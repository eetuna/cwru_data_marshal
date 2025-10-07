#include <iostream>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <mutex>
#include <deque>
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
    history.has_bounds = true;
}

static void record_pose_point(double x, double y, double z)
{
    std::scoped_lock lk(g_viz_mutex);
    auto &history = g_pose_history;
    history.points.emplace_back(x, y, z);
    if (!history.has_bounds)
    {
        history.min_x = history.max_x = x;
        history.min_y = history.max_y = y;
        history.has_bounds = true;
    }
    else
    {
        history.min_x = std::min(history.min_x, x);
        history.max_x = std::max(history.max_x, x);
        history.min_y = std::min(history.min_y, y);
        history.max_y = std::max(history.max_y, y);
    }

    if (history.points.size() > kMaxPoseTrail)
    {
        history.points.pop_front();
        recompute_pose_bounds(history);
    }

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

static void update_volume_image(const std::vector<float> &voxels,
                                size_t nx,
                                size_t ny,
                                size_t nz,
                                size_t frame_idx,
                                size_t frame_count)
{
    if (voxels.empty() || nx == 0 || ny == 0)
        return;

    std::vector<float> projection(nx * ny, std::numeric_limits<float>::lowest());
    for (size_t z = 0; z < nz; ++z)
    {
        for (size_t y = 0; y < ny; ++y)
        {
            for (size_t x = 0; x < nx; ++x)
            {
                size_t idx = z * ny * nx + y * nx + x;
                auto &dst = projection[y * nx + x];
                dst = std::max(dst, voxels[idx]);
            }
        }
    }

    auto [min_it, max_it] = std::minmax_element(projection.begin(), projection.end());
    float min_v = (min_it != projection.end()) ? *min_it : 0.f;
    float max_v = (max_it != projection.end()) ? *max_it : 1.f;
    float span = std::max(1e-6f, max_v - min_v);

    cv::Mat gray(static_cast<int>(ny), static_cast<int>(nx), CV_8UC1);
    for (size_t y = 0; y < ny; ++y)
    {
        auto *row = gray.ptr<uint8_t>(static_cast<int>(y));
        for (size_t x = 0; x < nx; ++x)
        {
            float norm = (projection[y * nx + x] - min_v) / span;
            norm = std::clamp(norm, 0.0f, 1.0f);
            row[x] = static_cast<uint8_t>(norm * 255.f);
        }
    }

    cv::Mat bgr;
    cv::cvtColor(gray, bgr, cv::COLOR_GRAY2BGR);

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

    cv::Point prev;
    bool first = true;
    size_t count = history.points.size();
    size_t idx = 0;
    for (const auto &pt : history.points)
    {
        cv::Point curr;
        if (!project_pose_to_frame(pt, history, frame, curr))
            return;

        if (!first)
        {
            double alpha = (count > 1) ? static_cast<double>(idx) / (count - 1) : 1.0;
            cv::Scalar color(0, static_cast<int>(255 * (1.0 - alpha)), static_cast<int>(255 * alpha));
            cv::line(frame, prev, curr, color, 2, cv::LINE_AA);
        }

        prev = curr;
        first = false;
        ++idx;
    }

    cv::circle(frame, prev, 5, cv::Scalar(0, 0, 255), -1, cv::LINE_AA);
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

        {
            std::scoped_lock lk(g_viz_mutex);
            if (!g_latest_image.empty())
                g_latest_image.copyTo(frame);
            history_snapshot = g_pose_history;
            status = g_status_text;
            pose = g_last_pose;
            has_pose = g_has_pose;
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

        int key = cv::waitKey(30);
        if (key == 27) // ESC
        {
            g_display_running.store(false);
        }
    }

    cv::destroyWindow("viz_client");
}

static void inspect_swmr_file(const std::string &path)
{
    if (path.empty())
        return;
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
    if (fapl < 0)
        return;
    H5Pset_libver_bounds(fapl, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
    H5Pset_fclose_degree(fapl, H5F_CLOSE_SEMI);
    hid_t file = H5Fopen(path.c_str(), H5F_ACC_RDONLY | H5F_ACC_SWMR_READ, fapl);
    H5Pclose(fapl);
    if (file < 0)
    {
        std::cerr << "viz: unable to open SWMR file " << path << "\n";
        return;
    }
    hid_t dset = H5Dopen2(file, "/images/data", H5P_DEFAULT);
    if (dset < 0)
    {
        std::cerr << "viz: /images/data dataset missing in " << path << "\n";
        H5Fclose(file);
        return;
    }
    if (H5Drefresh(dset) < 0)
    {
        std::cerr << "viz: H5Drefresh failed for " << path << "\n";
    }
    hid_t space = H5Dget_space(dset);
    if (space < 0)
    {
        std::cerr << "viz: cannot read dataset space" << std::endl;
        H5Dclose(dset);
        H5Fclose(file);
        return;
    }
    int rank = H5Sget_simple_extent_ndims(space);
    std::vector<hsize_t> dims(rank, 0);
    if (rank > 0)
        H5Sget_simple_extent_dims(space, dims.data(), nullptr);

    if (dims.size() == 5 && dims[0] > 0)
    {
        const hsize_t frames = dims[0];
        const hsize_t channels = dims[1];
        const hsize_t nz = dims[2];
        const hsize_t ny = dims[3];
        const hsize_t nx = dims[4];

        hsize_t mem_dims[5] = {1, 1, nz, ny, nx};
        hid_t memspace = H5Screate_simple(5, mem_dims, nullptr);
        if (memspace >= 0)
        {
            std::vector<float> voxels(static_cast<size_t>(nz * ny * nx));
            hsize_t start[5] = {frames - 1, 0, 0, 0, 0};
            hsize_t count[5] = {1, 1, nz, ny, nx};
            if (H5Sselect_hyperslab(space, H5S_SELECT_SET, start, nullptr, count, nullptr) >= 0)
            {
                if (channels > 1)
                {
                    std::cerr << "viz: only first channel rendered (" << channels << " available)\n";
                }
                if (H5Dread(dset, H5T_NATIVE_FLOAT, memspace, space, H5P_DEFAULT, voxels.data()) >= 0)
                {
                    update_volume_image(voxels, nx, ny, nz, frames - 1, frames);
                }
                else
                {
                    std::cerr << "viz: H5Dread failed for " << path << "\n";
                }
            }
            H5Sclose(memspace);
        }
        std::cout << "viz swmr: frames=" << frames
                  << " channels=" << channels
                  << " shape=" << nx << "x" << ny << "x" << nz
                  << "\n";
    }

    H5Sclose(space);
    H5Dclose(dset);
    H5Fclose(file);
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

    fs::path latest = fs::path(data) / "latest.json";
    if (!fs::exists(latest))
    {
        std::cerr << "viz: waiting for latest.json...\n";
        while (!fs::exists(latest))
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }

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
                        std::cout << "viz latest=" << lj.dump() << "\n";
                        if (lj.contains("frame_index") && lj.contains("path"))
                        {
                            try
                            {
                                inspect_swmr_file(lj["path"].get<std::string>());
                            }
                            catch (...)
                            {
                            }
                        }
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
                if (j.contains("frame_index") && j.contains("path"))
                {
                    try
                    {
                        inspect_swmr_file(j["path"].get<std::string>());
                    }
                    catch (...)
                    {
                    }
                }
            }
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "viz: WS error: " << e.what() << "\n";
    }

    g_display_running.store(false);
    if (poll.joinable())
        poll.join();
    if (display_thread.joinable())
        display_thread.join();
    return 0;
}
