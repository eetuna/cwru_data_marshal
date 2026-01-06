#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <set>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <memory>
#include <map>
#include <vector>
#include <cmath>
#include <limits>
#include <algorithm>

#include <hdf5.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>

using json = nlohmann::json;

// ============================================================================
// Task: A frame index to render
// ============================================================================
struct FrameTask
{
    uint64_t frame_index;
    std::string file_path;
};

// ============================================================================
// FrameQueue: Thread-safe FIFO queue for frame tasks
// ============================================================================
class FrameQueue
{
public:
    void push(FrameTask task)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push(std::move(task));
        cv_.notify_one();
    }

    bool pop(FrameTask &task)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stop_ || !pending_.empty(); });
        if (pending_.empty())
            return false;
        task = std::move(pending_.front());
        pending_.pop();
        return true;
    }

    void shutdown()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        cv_.notify_all();
    }

private:
    std::queue<FrameTask> pending_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_{false};
};

// ============================================================================
// SwmrHandle: HDF5 SWMR file reader
// ============================================================================
class SwmrHandle
{
public:
    explicit SwmrHandle(std::string path) : path_(std::move(path)) {}
    ~SwmrHandle() { close(); }

    SwmrHandle(const SwmrHandle &) = delete;
    SwmrHandle &operator=(const SwmrHandle &) = delete;

    // Read a single 2D slice from a 3D frame
    // Returns: vector of nx*ny floats, or empty on failure
    // frame_index: which frame (time dimension)
    // slice_index: which z-slice (0 to nz-1)
    std::vector<float> read_slice(uint64_t frame_index, int slice_index,
                                   size_t &nx, size_t &ny, size_t &nz)
    {
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            if (!ensure_open())
                return {};

            // Refresh to see latest data
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

            // Check dimensionality
            if (H5Sget_simple_extent_ndims(space) != 5)
            {
                H5Sclose(space);
                close();
                return {};
            }

            // Get dimensions: [frames, channels, z, y, x]
            hsize_t dims[5] = {0};
            if (H5Sget_simple_extent_dims(space, dims, nullptr) < 0)
            {
                H5Sclose(space);
                close();
                continue;
            }

            size_t frames = dims[0];
            size_t channels = dims[1];
            nz = dims[2];
            ny = dims[3];
            nx = dims[4];

            if (frames == 0 || nx == 0 || ny == 0 || nz == 0)
            {
                H5Sclose(space);
                return {};
            }

            // Clamp frame and slice indices
            if (frame_index >= frames)
                frame_index = frames - 1;
            if (slice_index < 0 || static_cast<size_t>(slice_index) >= nz)
                slice_index = nz / 2;  // Default to middle slice

            // Select hyperslab: single slice from frame
            hsize_t start[5] = {frame_index, 0, static_cast<hsize_t>(slice_index), 0, 0};
            hsize_t count[5] = {1, 1, 1, ny, nx};

            if (H5Sselect_hyperslab(space, H5S_SELECT_SET, start, nullptr, count, nullptr) < 0)
            {
                H5Sclose(space);
                close();
                continue;
            }

            // Create memory space
            hsize_t mem_dims[5] = {1, 1, 1, ny, nx};
            hid_t memspace = H5Screate_simple(5, mem_dims, nullptr);
            if (memspace < 0)
            {
                H5Sclose(space);
                close();
                continue;
            }

            // Read data
            std::vector<float> buffer(nx * ny);
            if (H5Dread(dataset_, H5T_NATIVE_FLOAT, memspace, space, H5P_DEFAULT,
                        buffer.data()) < 0)
            {
                H5Sclose(memspace);
                H5Sclose(space);
                close();
                continue;
            }

            H5Sclose(memspace);
            H5Sclose(space);
            return buffer;
        }

        return {};
    }

private:
    std::string path_;
    hid_t file_{-1};
    hid_t dataset_{-1};

    bool ensure_open()
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

        return true;
    }

    void close()
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
};

// ============================================================================
// DisplayState: Thread-safe display buffer
// ============================================================================
struct DisplayState
{
    std::mutex mutex;
    cv::Mat image;            // Latest rendered image (8UC1 grayscale)
    uint64_t frame_index{0};
    uint64_t total_frames{0};
    int current_slice{0};
    int total_slices{0};
    double fps{0.0};
    std::string status;

    void update(const cv::Mat &img, uint64_t fi, uint64_t tf, int cs, int ts, double f, const std::string &st)
    {
        std::lock_guard<std::mutex> lock(mutex);
        image = img.clone();
        frame_index = fi;
        total_frames = tf;
        current_slice = cs;
        total_slices = ts;
        fps = f;
        status = st;
    }

    void read(cv::Mat &img, uint64_t &fi, uint64_t &tf, int &cs, int &ts, double &f, std::string &st)
    {
        std::lock_guard<std::mutex> lock(mutex);
        img = image.clone();
        fi = frame_index;
        tf = total_frames;
        cs = current_slice;
        ts = total_slices;
        f = fps;
        st = status;
    }
};

// ============================================================================
// Global state
// ============================================================================
static std::atomic<bool> g_running{true};
static FrameQueue g_frame_queue;
static DisplayState g_display;
static std::atomic<int> g_user_slice_offset{0};  // User navigates with +/-

// ============================================================================
// HTTP Utilities
// ============================================================================
static size_t http_write_callback(void *contents, size_t size, size_t nmemb, std::string *s)
{
    s->append((char *)contents, size * nmemb);
    return size * nmemb;
}

static json http_get(const std::string &url)
{
    CURL *curl = curl_easy_init();
    if (!curl)
        return json::object();

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
        return json::object();

    try
    {
        return json::parse(response);
    }
    catch (...)
    {
        return json::object();
    }
}

// ============================================================================
// Frame Discovery Thread - Queries HTTP, fills gaps by querying HDF5
// ============================================================================
static void frame_discovery_loop(const std::string &http_url)
{
    std::cout << "viz: Starting frame discovery\n";
    std::cout << "viz: HTTP polling every 20ms + HDF5 gap-fill strategy\n";

    std::set<uint64_t> enqueued_frames;
    std::string current_file_path;

    while (g_running.load())
    {
        try
        {
            // Get current frame info from HTTP
            json j = http_get(http_url);
            if (j.is_object() && j.contains("data") && j["data"].is_object())
            {
                auto data = j["data"];
                if (data.contains("frame_index") && data.contains("path"))
                {
                    uint64_t latest_idx = data["frame_index"].get<uint64_t>();
                    std::string path = data["path"].get<std::string>();

                    if (path != current_file_path)
                    {
                        current_file_path = path;
                        std::cout << "viz: File: " << path << "\n";
                    }

                    // Open HDF5 to check total frames available
                    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
                    if (fapl >= 0)
                    {
                        H5Pset_libver_bounds(fapl, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
                        H5Pset_fclose_degree(fapl, H5F_CLOSE_SEMI);

                        hid_t file = H5Fopen(path.c_str(), H5F_ACC_RDONLY | H5F_ACC_SWMR_READ, fapl);
                        H5Pclose(fapl);

                        if (file >= 0)
                        {
                            hid_t dset = H5Dopen2(file, "/images/data", H5P_DEFAULT);
                            if (dset >= 0)
                            {
                                // Refresh to see latest
                                H5Drefresh(dset);
                                hid_t space = H5Dget_space(dset);
                                if (space >= 0)
                                {
                                    hsize_t dims[5] = {0};
                                    H5Sget_simple_extent_dims(space, dims, nullptr);
                                    uint64_t total_frames = dims[0];

                                    // Enqueue all frames from 0 to total_frames
                                    for (uint64_t idx = 0; idx < total_frames; ++idx)
                                    {
                                        if (enqueued_frames.find(idx) == enqueued_frames.end())
                                        {
                                            enqueued_frames.insert(idx);
                                            FrameTask task{idx, path};
                                            g_frame_queue.push(std::move(task));

                                            if (idx % 25 == 0)
                                                std::cout << "viz: enqueued frame " << idx << " (total=" << total_frames << ")\n";
                                        }
                                    }

                                    H5Sclose(space);
                                }
                                H5Dclose(dset);
                            }
                            H5Fclose(file);
                        }
                    }
                }
            }
        }
        catch (const std::exception &e)
        {
            std::cerr << "viz: Discovery error: " << e.what() << "\n";
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// ============================================================================
// Frame Worker Thread
// ============================================================================
static void frame_worker_loop()
{
    std::cout << "viz: Frame worker started\n";

    std::map<std::string, std::shared_ptr<SwmrHandle>> swmr_cache;
    uint64_t total_frames_seen = 0;
    int last_slice = -1;
    double smoothed_fps = 0.0;
    auto frame_start = std::chrono::steady_clock::now();

    FrameTask task;
    while (g_frame_queue.pop(task))
    {
        total_frames_seen = std::max(total_frames_seen, task.frame_index + 1);

        // Get or create SWMR handle for this file
        if (swmr_cache.find(task.file_path) == swmr_cache.end())
            swmr_cache[task.file_path] = std::make_shared<SwmrHandle>(task.file_path);

        auto handle = swmr_cache[task.file_path];

        // Determine current slice (user can navigate with g_user_slice_offset)
        size_t nx = 0, ny = 0, nz = 0;
        std::vector<float> dummy = handle->read_slice(task.frame_index, 0, nx, ny, nz);
        if (dummy.empty())
            continue;

        int current_slice = (nz / 2) + g_user_slice_offset.load();
        if (current_slice < 0)
            current_slice = 0;
        if (current_slice >= static_cast<int>(nz))
            current_slice = nz - 1;

        // Read the requested slice
        std::vector<float> slice_data = handle->read_slice(task.frame_index, current_slice, nx, ny, nz);
        if (slice_data.empty())
            continue;

        // Normalize and convert to 8UC1
        float min_val = *std::min_element(slice_data.begin(), slice_data.end());
        float max_val = *std::max_element(slice_data.begin(), slice_data.end());
        float range = (max_val > min_val) ? (max_val - min_val) : 1.0f;

        cv::Mat slice_mat(static_cast<int>(ny), static_cast<int>(nx), CV_32F, slice_data.data());
        cv::Mat normalized;
        slice_mat.convertTo(normalized, CV_32F, 1.0f / range, -min_val / range);
        cv::Mat uint8_image;
        normalized.convertTo(uint8_image, CV_8U, 255.0);

        // Update display state
        std::ostringstream status;
        status << "Frame " << (task.frame_index + 1) << "/" << total_frames_seen
               << "  Slice " << (current_slice + 1) << "/" << nz
               << "  Size=" << nx << "x" << ny;

        // Calculate FPS
        auto now = std::chrono::steady_clock::now();
        double delta = std::chrono::duration<double>(now - frame_start).count();
        if (delta > 0.1)  // Update every 0.1s
        {
            smoothed_fps = 1.0 / delta;
            frame_start = now;
        }

        g_display.update(uint8_image, task.frame_index, total_frames_seen,
                         current_slice, static_cast<int>(nz), smoothed_fps, status.str());
    }

    std::cout << "viz: Frame worker done\n";
}

// ============================================================================
// Display Thread (Main OpenCV Loop)
// ============================================================================
static void display_loop()
{
    std::cout << "viz: Display thread started\n";

    cv::namedWindow("viz_client", cv::WINDOW_AUTOSIZE);

    double display_fps = 0.0;
    int display_frame_count = 0;
    auto display_start = std::chrono::steady_clock::now();

    while (g_running.load())
    {
        cv::Mat img;
        uint64_t fi = 0, tf = 0;
        int cs = 0, ts = 0;
        double fps = 0.0;
        std::string status;

        g_display.read(img, fi, tf, cs, ts, fps, status);

        // Build display frame
        cv::Mat display_frame;
        if (img.empty())
        {
            display_frame = cv::Mat::zeros(240, 320, CV_8UC3);
            cv::putText(display_frame, "Waiting for frames...", cv::Point(20, 120),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 1);
        }
        else
        {
            // Convert grayscale to BGR for display
            cv::cvtColor(img, display_frame, cv::COLOR_GRAY2BGR);
        }

        // Draw status text
        if (!status.empty())
        {
            cv::putText(display_frame, status, cv::Point(10, 25),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0), 2);
            cv::putText(display_frame, status, cv::Point(10, 25),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 1);
        }

        // Draw FPS
        std::ostringstream fps_str;
        fps_str << std::fixed << std::setprecision(1) << fps << " fps";
        cv::putText(display_frame, fps_str.str(), cv::Point(10, 50),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0), 2);
        cv::putText(display_frame, fps_str.str(), cv::Point(10, 50),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 255, 0), 1);

        // Draw navigation hint
        cv::putText(display_frame, "Slice: UP/DOWN arrows", cv::Point(10, display_frame.rows - 20),
                    cv::FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(200, 200, 200), 1);

        cv::imshow("viz_client", display_frame);

        // Measure display FPS
        display_frame_count++;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - display_start).count();
        if (elapsed >= 1.0)
        {
            display_fps = display_frame_count / elapsed;
            std::cerr << "viz: display_fps=" << std::fixed << std::setprecision(1) << display_fps << "\n";
            display_start = now;
            display_frame_count = 0;
        }

        // Handle keyboard
        int key = cv::waitKey(10);
        if (key == 27)  // ESC
        {
            std::cout << "viz: User pressed ESC\n";
            g_running.store(false);
        }
        else if (key == 82)  // UP arrow
        {
            g_user_slice_offset.fetch_add(1);
            std::cout << "viz: Slice offset +1 = " << g_user_slice_offset.load() << "\n";
        }
        else if (key == 84)  // DOWN arrow
        {
            g_user_slice_offset.fetch_sub(1);
            std::cout << "viz: Slice offset -1 = " << g_user_slice_offset.load() << "\n";
        }
    }

    cv::destroyWindow("viz_client");
    std::cout << "viz: Display thread done\n";
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char **argv)
{
    std::string http_url = "http://localhost:8080/v1/mrd/latest";
    std::string data_dir = "./data/mrd";

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if ((arg == "--http" || arg == "-http") && i + 1 < argc)
            http_url = argv[++i];
        else if ((arg == "--data" || arg == "-data") && i + 1 < argc)
            data_dir = argv[++i];
    }

    std::cout << "=== viz_client - Single-Slice Navigator ===\n";
    std::cout << "HTTP URL: " << http_url << "\n";
    std::cout << "Data dir: " << data_dir << "\n\n";

    // Start threads
    std::thread discovery_thread(frame_discovery_loop, http_url);
    std::thread worker_thread(frame_worker_loop);
    std::thread display_thread(display_loop);

    // Wait for display thread (main thread)
    display_thread.join();

    // Signal other threads to stop
    g_running.store(false);
    g_frame_queue.shutdown();

    discovery_thread.join();
    worker_thread.join();

    std::cout << "viz: All threads joined. Exiting.\n";
    return 0;
}
