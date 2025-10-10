#include <iostream>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <unordered_map>
#include <sstream>
#include <iomanip>
#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <hdf5.h>
#include <vector>
#include <algorithm>
#include <limits>
#include <cmath>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;
namespace websocket = boost::beast::websocket;

static std::time_t file_time_to_time_t(std::filesystem::file_time_type t)
{
    using namespace std::chrono;
    // Convert file_clock -> system_clock
    const auto sctp = time_point_cast<system_clock::duration>(
        t - std::filesystem::file_time_type::clock::now() + system_clock::now());
    return system_clock::to_time_t(sctp);
}

struct PoseHistory
{
    std::deque<cv::Point3d> points;
    double min_x = 0.0;
    double max_x = 0.0;
    double min_y = 0.0;
    double max_y = 0.0;
    bool has_bounds = false;
};

static std::mutex g_viz_mutex;
static cv::Mat g_latest_image;
static PoseHistory g_pose_history;
static std::array<double, 3> g_last_pose{0.0, 0.0, 0.0};
static bool g_has_pose = false;
static std::string g_status_text;
static constexpr size_t kMaxPoseTrail = 256;
static std::atomic<bool> g_display_running{true};
static double g_recent_fps = 0.0;
static std::chrono::steady_clock::time_point g_last_frame_time;
static bool g_have_last_frame_time = false;

static void update_volume_image(const std::vector<float> &voxels,
                                size_t nx,
                                size_t ny,
                                size_t nz,
                                size_t frame_idx,
                                size_t frame_count);

struct FrameTask
{
    std::string path;
    size_t frame_index{0};
    size_t advertised_frames{0};
};

class FrameQueue
{
  public:
    void push(FrameTask task)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_[task.path] = std::move(task);
        cv_.notify_one();
    }

    bool pop(FrameTask &task)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return stop_ || !pending_.empty(); });
        if (pending_.empty())
            return false;
        auto it = pending_.begin();
        task = std::move(it->second);
        pending_.erase(it);
        return true;
    }

    void shutdown()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        cv_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<std::string, FrameTask> pending_;
    bool stop_{false};
};

class SwmrHandle
{
  public:
    explicit SwmrHandle(std::string path) : path_(std::move(path)) {}
    ~SwmrHandle() { close(); }

    SwmrHandle(const SwmrHandle &) = delete;
    SwmrHandle &operator=(const SwmrHandle &) = delete;

    bool render_latest(size_t frame_index_hint, size_t advertised_frames);

  private:
    std::string path_;
    hid_t file_{-1};
    hid_t dataset_{-1};
    std::array<hsize_t, 5> dims_{};
    hsize_t last_frame_{static_cast<hsize_t>(-1)};
    bool warned_channel_{false};
    bool logged_shape_{false};
    std::vector<float> buffer_;

    bool ensure_open();
    void close();
};

class SwmrCache
{
  public:
    std::shared_ptr<SwmrHandle> get(const std::string &path)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = handles_.find(path);
        if (it != handles_.end())
            return it->second;
        auto handle = std::make_shared<SwmrHandle>(path);
        handles_.emplace(path, handle);
        return handle;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handles_.clear();
    }

  private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<SwmrHandle>> handles_;
};

static FrameQueue g_frame_queue;
static SwmrCache g_swmr_cache;

static void recompute_pose_bounds(PoseHistory &history)
{
    if (history.points.empty())
    {
        history.min_x = history.max_x = 0.0;
        history.min_y = history.max_y = 0.0;
        history.has_bounds = false;
        return;
    }

    history.min_x = history.max_x = history.points.front().x;
    history.min_y = history.max_y = history.points.front().y;
    for (const auto &pt : history.points)
    {
        history.min_x = std::min(history.min_x, pt.x);
        history.max_x = std::max(history.max_x, pt.x);
        history.min_y = std::min(history.min_y, pt.y);
        history.max_y = std::max(history.max_y, pt.y);
    }

    constexpr double kMinExtent = 1e-6;
    if (std::abs(history.max_x - history.min_x) < kMinExtent)
    {
        double center = history.min_x;
        history.min_x = center - 0.5 * kMinExtent;
        history.max_x = center + 0.5 * kMinExtent;
    }
    if (std::abs(history.max_y - history.min_y) < kMinExtent)
    {
        double center = history.min_y;
        history.min_y = center - 0.5 * kMinExtent;
        history.max_y = center + 0.5 * kMinExtent;
    }
    history.has_bounds = true;
}

static void record_pose_point(double x, double y, double z)
{
    std::scoped_lock lk(g_viz_mutex);
    auto &history = g_pose_history;
    history.points.emplace_back(x, y, z);
    if (history.points.size() > kMaxPoseTrail)
        history.points.pop_front();
    recompute_pose_bounds(history);

    g_last_pose = {x, y, z};
    g_has_pose = true;
}

static void record_pose_from_json(const json &j)
{
    if (!j.contains("p") || !j["p"].is_array())
        return;
    const auto &arr = j["p"];
    if (arr.size() < 2)
        return;
    double x = arr[0].get<double>();
    double y = arr[1].get<double>();
    double z = (arr.size() > 2) ? arr[2].get<double>() : 0.0;
    record_pose_point(x, y, z);
}

bool SwmrHandle::ensure_open()
{
    if (file_ >= 0 && dataset_ >= 0)
        return true;

    close();

    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
    if (fapl < 0)
        return false;
    H5Pset_libver_bounds(fapl, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
    H5Pset_fclose_degree(fapl, H5F_CLOSE_SEMI);

    file_ = H5Fopen(path_.c_str(), H5F_ACC_RDONLY | H5F_ACC_SWMR_READ, fapl);
    H5Pclose(fapl);
    if (file_ < 0)
        return false;

    dataset_ = H5Dopen2(file_, "/images/data", H5P_DEFAULT);
    if (dataset_ < 0)
    {
        close();
        return false;
    }

    last_frame_ = static_cast<hsize_t>(-1);
    warned_channel_ = false;
    logged_shape_ = false;
    return true;
}

void SwmrHandle::close()
{
    if (dataset_ >= 0)
    {
        H5Dclose(dataset_);
        dataset_ = -1;
    }
    if (file_ >= 0)
    {
        H5Fclose(file_);
        file_ = -1;
    }
}

bool SwmrHandle::render_latest(size_t frame_index_hint, size_t advertised_frames)
{
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (!ensure_open())
            return false;

        if (H5Drefresh(dataset_) < 0)
        {
            close();
            continue;
        }

        hid_t space = H5Dget_space(dataset_);
        if (space < 0)
        {
            close();
            continue;
        }

        if (H5Sget_simple_extent_ndims(space) != 5)
        {
            H5Sclose(space);
            close();
            return false;
        }

        std::array<hsize_t, 5> dims{};
        if (H5Sget_simple_extent_dims(space, dims.data(), nullptr) < 0)
        {
            H5Sclose(space);
            close();
            continue;
        }

        hsize_t frames = dims[0];
        if (frames == 0)
        {
            H5Sclose(space);
            return false;
        }

        hsize_t target = static_cast<hsize_t>(frame_index_hint);
        if (target >= frames)
            target = frames - 1;
        if (target == last_frame_)
        {
            H5Sclose(space);
            return false;
        }

        if (dims[1] > 1 && !warned_channel_)
        {
            std::cerr << "viz: only first channel rendered (" << dims[1] << " available)\n";
            warned_channel_ = true;
        }

        if (!logged_shape_ || dims != dims_)
        {
            std::cout << "viz swmr: frames=" << frames
                      << " channels=" << dims[1]
                      << " shape=" << dims[4] << "x" << dims[3] << "x" << dims[2]
                      << "\n";
            dims_ = dims;
            logged_shape_ = true;
        }

        hsize_t start[5] = {target, 0, 0, 0, 0};
        hsize_t count[5] = {1, 1, dims[2], dims[3], dims[4]};
        if (H5Sselect_hyperslab(space, H5S_SELECT_SET, start, nullptr, count, nullptr) < 0)
        {
            H5Sclose(space);
            close();
            continue;
        }

        hsize_t mem_dims[5] = {1, 1, dims[2], dims[3], dims[4]};
        hid_t memspace = H5Screate_simple(5, mem_dims, nullptr);
        if (memspace < 0)
        {
            H5Sclose(space);
            close();
            continue;
        }

        buffer_.resize(static_cast<size_t>(dims[2] * dims[3] * dims[4]));
        if (H5Dread(dataset_, H5T_NATIVE_FLOAT, memspace, space, H5P_DEFAULT, buffer_.data()) < 0)
        {
            H5Sclose(memspace);
            H5Sclose(space);
            close();
            continue;
        }

        H5Sclose(memspace);
        H5Sclose(space);

        last_frame_ = target;
        size_t total_frames = std::max<size_t>(static_cast<size_t>(frames), advertised_frames);
        update_volume_image(buffer_, static_cast<size_t>(dims[4]), static_cast<size_t>(dims[3]), static_cast<size_t>(dims[2]),
                            static_cast<size_t>(target), total_frames);
        return true;
    }

    return false;
}

static void update_volume_image(const std::vector<float> &voxels,
                                size_t nx,
                                size_t ny,
                                size_t nz,
                                size_t frame_idx,
                                size_t frame_count)
{
    if (voxels.empty() || nx == 0 || ny == 0)
        return;

    const size_t plane = nx * ny;
    std::vector<float> projection(plane, std::numeric_limits<float>::lowest());
    for (size_t z = 0; z < nz; ++z)
    {
        const float *src = voxels.data() + z * plane;
        for (size_t i = 0; i < plane; ++i)
            projection[i] = std::max(projection[i], src[i]);
    }

    cv::Mat proj_mat(static_cast<int>(ny), static_cast<int>(nx), CV_32F, projection.data());
    double min_v = 0.0;
    double max_v = 0.0;
    cv::minMaxLoc(proj_mat, &min_v, &max_v);
    double scale = (max_v > min_v) ? 255.0 / (max_v - min_v) : 1.0;
    double shift = -min_v * scale;

    cv::Mat gray;
    proj_mat.convertTo(gray, CV_8U, scale, shift);

    cv::Mat bgr;
    cv::cvtColor(gray, bgr, cv::COLOR_GRAY2BGR);

    static double smoothed_fps_internal = 0.0;
    double smoothed_fps = smoothed_fps_internal;
    double instant_fps = 0.0;
    const auto now = std::chrono::steady_clock::now();
    if (g_have_last_frame_time)
    {
        const double delta = std::chrono::duration<double>(now - g_last_frame_time).count();
        if (delta > 1e-6)
            instant_fps = 1.0 / delta;
    }
    g_last_frame_time = now;
    g_have_last_frame_time = true;
    if (instant_fps > 0.0)
    {
        if (smoothed_fps <= 0.0)
            smoothed_fps = instant_fps;
        else
            smoothed_fps = 0.8 * smoothed_fps + 0.2 * instant_fps;
    }
    else if (smoothed_fps > 0.0)
    {
        smoothed_fps = 0.95 * smoothed_fps;
        if (smoothed_fps < 0.01)
            smoothed_fps = 0.0;
    }
    smoothed_fps_internal = smoothed_fps;

    std::ostringstream oss;
    oss << "Frame " << (frame_idx + 1) << "/" << frame_count
        << "  size=" << nx << "x" << ny << "x" << nz
        << "  max-projection"
        << "  intensity=[" << std::fixed << std::setprecision(3)
        << min_v << ", " << max_v << "]";

    {
        std::scoped_lock lk(g_viz_mutex);
        g_latest_image = std::move(bgr);
        g_status_text = oss.str();
        g_recent_fps = smoothed_fps;
    }
}

static void frame_worker_loop()
{
    FrameTask task;
    while (g_frame_queue.pop(task))
    {
        auto handle = g_swmr_cache.get(task.path);
        if (!handle)
            continue;
        handle->render_latest(task.frame_index, task.advertised_frames);
    }
}

static void enqueue_frame_from_json(const json &j)
{
    try
    {
        if (!j.contains("path") || !j["path"].is_string())
            return;
        if (j.contains("flushed") && j["flushed"].is_boolean() && !j["flushed"].get<bool>())
            return;

        FrameTask task;
        task.path = j["path"].get<std::string>();
        if (j.contains("frame_index") && j["frame_index"].is_number_unsigned())
            task.frame_index = j["frame_index"].get<uint64_t>();
        task.advertised_frames = task.frame_index + 1;
        g_frame_queue.push(std::move(task));
    }
    catch (...)
    {
    }
}

static bool project_pose_to_frame(const cv::Point3d &pt,
                                  const PoseHistory &history,
                                  const cv::Mat &frame,
                                  cv::Point &out)
{
    if (frame.empty() || !history.has_bounds)
        return false;

    double range_x = history.max_x - history.min_x;
    double range_y = history.max_y - history.min_y;
    if (std::abs(range_x) < 1e-9)
        range_x = 1.0;
    if (std::abs(range_y) < 1e-9)
        range_y = 1.0;

    double norm_x = (pt.x - history.min_x) / range_x;
    double norm_y = (pt.y - history.min_y) / range_y;
    norm_x = std::clamp(norm_x, 0.0, 1.0);
    norm_y = std::clamp(norm_y, 0.0, 1.0);

    int width = std::max(frame.cols, 1);
    int height = std::max(frame.rows, 1);
    int px = static_cast<int>(norm_x * (width - 1));
    int py = static_cast<int>((1.0 - norm_y) * (height - 1));
    px = std::clamp(px, 0, width - 1);
    py = std::clamp(py, 0, height - 1);
    out = {px, py};
    return true;
}

static void draw_pose_overlay(cv::Mat &frame, const PoseHistory &history)
{
    if (frame.empty() || history.points.empty() || !history.has_bounds)
        return;

    const auto &latest = history.points.back();
    cv::Point pixel;
    if (!project_pose_to_frame(latest, history, frame, pixel))
        return;

    cv::circle(frame, pixel, 6, cv::Scalar(0, 0, 255), -1, cv::LINE_AA);
    cv::circle(frame, pixel, 10, cv::Scalar(0, 0, 255), 2, cv::LINE_AA);
}

static void display_loop()
{
    cv::namedWindow("viz_client", cv::WINDOW_AUTOSIZE);

    while (g_display_running.load())
    {
        cv::Mat frame;
        PoseHistory history_snapshot;
        std::string status;
        std::array<double, 3> pose{};
        bool has_pose = false;
        double fps_value = 0.0;

        {
            std::scoped_lock lk(g_viz_mutex);
            if (!g_latest_image.empty())
                g_latest_image.copyTo(frame);
            history_snapshot = g_pose_history;
            status = g_status_text;
            pose = g_last_pose;
            has_pose = g_has_pose;
            fps_value = g_recent_fps;
        }

        if (frame.empty())
        {
            frame = cv::Mat::zeros(240, 320, CV_8UC3);
            cv::putText(frame, "Waiting for MRD frames...", cv::Point(10, 120),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(200, 200, 200), 1, cv::LINE_AA);
        }
        else
        {
            draw_pose_overlay(frame, history_snapshot);
            if (!status.empty())
            {
                cv::putText(frame, status, cv::Point(10, 20), cv::FONT_HERSHEY_SIMPLEX, 0.45,
                            cv::Scalar(0, 0, 0), 2, cv::LINE_AA);
                cv::putText(frame, status, cv::Point(10, 20), cv::FONT_HERSHEY_SIMPLEX, 0.45,
                            cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
            }
            if (fps_value > 0.0)
            {
                std::ostringstream fps_oss;
                fps_oss << std::fixed << std::setprecision(1) << fps_value << " fps";
                const auto fps_text = fps_oss.str();
                cv::Point origin(10, status.empty() ? 20 : 40);
                if (!status.empty())
                {
                    cv::putText(frame, fps_text, origin, cv::FONT_HERSHEY_SIMPLEX, 0.45,
                                cv::Scalar(0, 0, 0), 2, cv::LINE_AA);
                }
                cv::putText(frame, fps_text, origin, cv::FONT_HERSHEY_SIMPLEX, 0.45,
                            cv::Scalar(0, 255, 255), 1, cv::LINE_AA);
            }
            if (has_pose)
            {
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(2)
                    << "Pose p=[" << pose[0] << ", " << pose[1] << ", " << pose[2] << "]";
                auto text = oss.str();
                int baseline = 0;
                auto size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 0.45, 1, &baseline);
                cv::Point origin(10, std::max(frame.rows - 10, size.height + 10));
                cv::putText(frame, text, origin, cv::FONT_HERSHEY_SIMPLEX, 0.45,
                            cv::Scalar(0, 0, 0), 2, cv::LINE_AA);
                cv::putText(frame, text, origin, cv::FONT_HERSHEY_SIMPLEX, 0.45,
                            cv::Scalar(0, 255, 0), 1, cv::LINE_AA);
            }
        }

        cv::imshow("viz_client", frame);

        int key = cv::waitKey(1);
        if (key == 27) // ESC
        {
            g_display_running.store(false);
        }
    }

    cv::destroyWindow("viz_client");
}

int main(int argc, char **argv)
{
    std::string ws_url = "ws://localhost:8090/ws";
    std::string data = "./data/mrd"; // sensible default

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--ws" && i + 1 < argc)
            ws_url = argv[++i];
        else if (a == "--data" && i + 1 < argc)
            data = argv[++i];
    }

    std::thread display_thread(display_loop);
    std::thread frame_thread(frame_worker_loop);

    fs::path latest = fs::path(data) / "latest.json";
    if (!fs::exists(latest))
        std::cerr << "viz: latest.json not found yet; waiting for frames while continuing setup...\n";

    // --- Poll latest.json forever ---
    std::thread poll([&]()
                     {
        std::time_t last = 0;
        while (g_display_running.load())
        {
            try
            {
                if (fs::exists(latest))
                {
                    auto wt = file_time_to_time_t(fs::last_write_time(latest));
                    if (wt != last)
                    {
                        std::ifstream lf(latest);
                        json lj;
                        lf >> lj;
                        bool log_entry = true;
                        if (lj.contains("frame_index") && lj["frame_index"].is_number_unsigned())
                        {
                            auto idx = lj["frame_index"].get<uint64_t>();
                            log_entry = (idx % 30 == 0);
                        }
                        if (log_entry)
                            std::cout << "viz latest=" << lj.dump() << "\n";
                        enqueue_frame_from_json(lj);
                        last = wt;
                    }
                }
            }
            catch (...)
            {
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });

    // --- Connect WS once, print until closed ---
    try
    {
        boost::asio::io_context ioc;
        auto pos = ws_url.find("//");
        auto hp = ws_url.substr(pos + 2);
        auto slash = hp.find("/");
        std::string host = hp.substr(0, hp.find(":"));
        std::string port = hp.substr(host.size() + 1, slash - host.size() - 1);
        std::string target = hp.substr(slash);

        boost::asio::ip::tcp::resolver res{ioc};
        auto results = res.resolve(host, port);
        boost::asio::ip::tcp::socket sock{ioc};
        boost::asio::connect(sock, results.begin(), results.end());
        websocket::stream<boost::asio::ip::tcp::socket> ws{std::move(sock)};
        ws.handshake(host + ":" + port, target);

        std::cout << "viz: WS connected " << ws_url << "\n";
        boost::beast::flat_buffer buf;
        while (g_display_running.load())
        {
            boost::system::error_code ec;
            ws.read(buf, ec);
            if (ec)
            {
                std::cerr << "viz: WS closed: " << ec.message() << "\n";
                break; // just exit WS loop, poll thread still runs
            }
            auto s = boost::beast::buffers_to_string(buf.data());
            buf.consume(buf.size());
            auto j = json::parse(s, nullptr, false);
            if (j.is_object())
            {
                bool log_entry = true;
                if (j.contains("frame_index") && j["frame_index"].is_number_unsigned())
                {
                    auto idx = j["frame_index"].get<uint64_t>();
                    log_entry = (idx % 30 == 0);
                }
                if (log_entry)
                    std::cout << "viz ws: " << j.dump() << "\n";
                if (j.contains("type") && j["type"].is_string())
                {
                    auto type = j["type"].get<std::string>();
                    if (type == "pose")
                    {
                        try
                        {
                            record_pose_from_json(j);
                        }
                        catch (...)
                        {
                        }
                    }
                }
                enqueue_frame_from_json(j);
            }
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "viz: WS error: " << e.what() << "\n";
    }

    g_display_running.store(false);
    g_frame_queue.shutdown();
    if (poll.joinable())
        poll.join();
    if (frame_thread.joinable())
        frame_thread.join();
    if (display_thread.joinable())
        display_thread.join();
    g_swmr_cache.clear();
    return 0;
}
