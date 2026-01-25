#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <algorithm>
#include <string>
#include <filesystem>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <future>

#include <hdf5.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>

using json = nlohmann::json;

// ============================================================================
// Cached HDF5 file handle for SWMR reading (avoids repeated open/close)
// ============================================================================
struct CachedHDF5Reader
{
    hid_t file_{-1};
    hid_t dataset_{-1};
    std::string current_path_;
    std::filesystem::file_time_type last_write_time_{};
    uintmax_t last_size_{0};

    ~CachedHDF5Reader()
    {
        close();
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
        current_path_.clear();
    }

    bool open(const std::string &path, bool force_reopen = false)
    {
        // If same file is already open, just refresh the dataset unless it changed
        if (!force_reopen && file_ >= 0 && current_path_ == path)
        {
            std::error_code ec_mtime;
            std::error_code ec_size;
            auto mtime = std::filesystem::last_write_time(path, ec_mtime);
            auto size = std::filesystem::file_size(path, ec_size);
            if (!ec_mtime && !ec_size && mtime == last_write_time_ && size == last_size_)
            {
                if (dataset_ >= 0)
                {
                    if (H5Drefresh(dataset_) >= 0)
                        return true;
                }
            }
        }

        // Close old file if different
        close();

        // Open new file in SWMR read mode
        hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
        if (fapl < 0)
            return false;

        H5Pset_libver_bounds(fapl, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
        H5Pset_fclose_degree(fapl, H5F_CLOSE_SEMI);

        file_ = H5Fopen(path.c_str(), H5F_ACC_RDONLY | H5F_ACC_SWMR_READ, fapl);
        H5Pclose(fapl);

        if (file_ < 0)
            return false;

        dataset_ = H5Dopen2(file_, "/images/data", H5P_DEFAULT);
        if (dataset_ < 0)
        {
            close();
            return false;
        }

        current_path_ = path;
        std::error_code ec_mtime;
        std::error_code ec_size;
        last_write_time_ = std::filesystem::last_write_time(path, ec_mtime);
        last_size_ = std::filesystem::file_size(path, ec_size);
        return true;
    }

    // Read a single slice - uses cached handles
    std::vector<float> read_slice(uint64_t frame_idx, int slice_idx,
                                   size_t &nx, size_t &ny, size_t &nz)
    {
        if (dataset_ < 0)
            return {};

        // Refresh to see latest writer data
        if (H5Drefresh(dataset_) < 0)
        {
            // Attempt to reopen if the writer recreated the file.
            const std::string path = current_path_;
            close();
            if (!path.empty())
            {
                if (!open(path, true))
                    return {};
            }
        }

        hid_t space = H5Dget_space(dataset_);
        if (space < 0)
            return {};

        // Get dimensions: [frames, channels, z, y, x]
        hsize_t dims[5] = {0};
        H5Sget_simple_extent_dims(space, dims, nullptr);

        size_t frames = dims[0];
        nz = dims[2];
        ny = dims[3];
        nx = dims[4];

        if (frames == 0 || nx == 0 || ny == 0 || nz == 0)
        {
            H5Sclose(space);
            return {};
        }

        // Clamp indices
        if (frame_idx >= frames)
            frame_idx = frames - 1;
        if (slice_idx < 0)
            slice_idx = 0;
        if ((size_t)slice_idx >= nz)
            slice_idx = nz - 1;

        // Select hyperslab: single slice from frame
        hsize_t start[5] = {frame_idx, 0, (hsize_t)slice_idx, 0, 0};
        hsize_t count[5] = {1, 1, 1, ny, nx};

        if (H5Sselect_hyperslab(space, H5S_SELECT_SET, start, nullptr, count, nullptr) < 0)
        {
            H5Sclose(space);
            return {};
        }

        // Create memory space
        hsize_t mem_dims[5] = {1, 1, 1, ny, nx};
        hid_t memspace = H5Screate_simple(5, mem_dims, nullptr);
        if (memspace < 0)
        {
            H5Sclose(space);
            return {};
        }

        // Read data
        std::vector<float> buffer(nx * ny);
        herr_t status = H5Dread(dataset_, H5T_NATIVE_FLOAT, memspace, space, H5P_DEFAULT, buffer.data());

        H5Sclose(memspace);
        H5Sclose(space);

        if (status < 0)
            return {};

        return buffer;
    }
};

// ============================================================================
// HTTP GET helper (reuses CURL handle for speed)
// ============================================================================
static size_t http_write_callback(void *contents, size_t size, size_t nmemb, std::string *s)
{
    s->append((char *)contents, size * nmemb);
    return size * nmemb;
}

static json http_get(CURL *curl, const std::string &url)
{
    if (!curl)
        return json::object();

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);

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
// Async data fetcher - runs HTTP + HDF5 reads on background thread
// ============================================================================
struct AsyncDataFetcher
{
    // Thread synchronization
    std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<bool> running_{true};
    std::thread worker_thread_;

    // Request state
    std::atomic<bool> request_pending_{false};
    std::string requested_url_;

    // Result state (protected by mtx_)
    bool result_ready_{false};
    std::string result_path_;
    uint64_t result_frame_idx_{0};
    std::vector<float> result_data_;
    size_t result_nx_{0}, result_ny_{0}, result_nz_{0};

    // Cached resources
    CURL *curl_{nullptr};
    CachedHDF5Reader hdf5_reader_;

    // Slice selection
    std::atomic<int> slice_offset_{0};

    AsyncDataFetcher(const std::string &url) : requested_url_(url)
    {
        curl_ = curl_easy_init();
        if (curl_)
        {
            curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, 100L);
            curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT_MS, 500L);
            curl_easy_setopt(curl_, CURLOPT_TCP_NODELAY, 1L);
            curl_easy_setopt(curl_, CURLOPT_TCP_KEEPALIVE, 1L);
        }

        worker_thread_ = std::thread(&AsyncDataFetcher::worker_loop, this);
    }

    ~AsyncDataFetcher()
    {
        running_ = false;
        cv_.notify_all();
        if (worker_thread_.joinable())
            worker_thread_.join();
        if (curl_)
            curl_easy_cleanup(curl_);
    }

    void worker_loop()
    {
        while (running_)
        {
            // Wait for work or shutdown
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait_for(lk, std::chrono::milliseconds(10), [this] {
                    return !running_ || request_pending_;
                });
            }

            if (!running_)
                break;

            if (!request_pending_)
                continue;

            // Do the actual I/O work
            json j = http_get(curl_, requested_url_);

            std::string path;
            uint64_t frame_idx = 0;
            std::vector<float> data;
            size_t nx = 0, ny = 0, nz = 0;

            if (j.contains("data") && j["data"].is_object())
            {
                auto &d = j["data"];
                if (d.contains("path") && d.contains("frame_index"))
                {
                    path = d["path"].get<std::string>();
                    frame_idx = d["frame_index"].get<uint64_t>();

                    // Open/refresh HDF5 file and read slice
                    if (hdf5_reader_.open(path))
                    {
                        int offset = slice_offset_.load();
                        int slice = (nz > 0) ? (int)(nz / 2) + offset : offset;
                        data = hdf5_reader_.read_slice(frame_idx, slice, nx, ny, nz);

                        // Recalculate slice with actual nz
                        if (!data.empty() && nz > 0)
                        {
                            slice = (int)(nz / 2) + offset;
                            if (slice < 0) slice = 0;
                            if ((size_t)slice >= nz) slice = nz - 1;
                            data = hdf5_reader_.read_slice(frame_idx, slice, nx, ny, nz);
                        }
                    }
                }
            }

            // Store result
            {
                std::lock_guard<std::mutex> lk(mtx_);
                result_path_ = path;
                result_frame_idx_ = frame_idx;
                result_data_ = std::move(data);
                result_nx_ = nx;
                result_ny_ = ny;
                result_nz_ = nz;
                result_ready_ = true;
                request_pending_ = false;
            }
        }
    }

    void request_update()
    {
        request_pending_ = true;
        cv_.notify_one();
    }

    bool try_get_result(std::string &path, uint64_t &frame_idx,
                        std::vector<float> &data, size_t &nx, size_t &ny, size_t &nz)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!result_ready_)
            return false;

        path = result_path_;
        frame_idx = result_frame_idx_;
        data = std::move(result_data_);
        nx = result_nx_;
        ny = result_ny_;
        nz = result_nz_;
        result_ready_ = false;
        return true;
    }

    void set_slice_offset(int offset)
    {
        slice_offset_ = offset;
    }

    int get_slice_offset() const
    {
        return slice_offset_.load();
    }
};

// ============================================================================
// Main - Non-blocking async design for responsive GUI
// ============================================================================
int main(int argc, char **argv)
{
    std::string http_url = "http://localhost:8080/v1/mrd/latest";

    // Parse args
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if ((arg == "--http" || arg == "-http") && i + 1 < argc)
            http_url = argv[++i];
    }

    std::cout << "=== viz_client - Async Non-Blocking Design ===\n";
    std::cout << "HTTP URL: " << http_url << "\n";
    std::cout << "Controls: UP/DOWN = slice, ESC = exit\n";
    std::cout << "Features: Async I/O, cached HDF5 handles, responsive GUI\n\n";

    cv::namedWindow("viz_client", cv::WINDOW_AUTOSIZE);

    // Create async fetcher (runs HTTP + HDF5 on background thread)
    AsyncDataFetcher fetcher(http_url);

    size_t nx = 0, ny = 0, nz = 0;
    uint64_t last_frame = UINT64_MAX;
    std::vector<float> current_slice_data;
    int current_slice = 0;

    // FPS tracking
    int frame_count = 0;
    auto fps_start = std::chrono::steady_clock::now();
    double current_fps = 0.0;

    // Request first update
    fetcher.request_update();
    auto last_request = std::chrono::steady_clock::now();

    while (true)
    {
        // 1. Check for new data from background thread (non-blocking)
        std::string path;
        uint64_t frame_idx;
        std::vector<float> new_data;
        size_t new_nx, new_ny, new_nz;

        if (fetcher.try_get_result(path, frame_idx, new_data, new_nx, new_ny, new_nz))
        {
            if (!new_data.empty())
            {
                current_slice_data = std::move(new_data);
                nx = new_nx;
                ny = new_ny;
                nz = new_nz;

                // Calculate current slice index
                int offset = fetcher.get_slice_offset();
                current_slice = (nz > 0) ? (int)(nz / 2) + offset : 0;
                if (current_slice < 0) current_slice = 0;
                if (nz > 0 && (size_t)current_slice >= nz) current_slice = nz - 1;

                // Update FPS counter only on NEW frames
                if (frame_idx != last_frame)
                {
                    frame_count++;
                    std::cout << "viz: frame " << frame_idx << " slice " << (current_slice + 1) << "/" << nz << "\n";
                    last_frame = frame_idx;
                }
            }
        }

        // 2. Request next update if enough time has passed (rate limit to ~100 Hz)
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_request).count() >= 10)
        {
            fetcher.request_update();
            last_request = now;
        }

        // 3. Render current frame (always responsive)
        if (!current_slice_data.empty() && nx > 0 && ny > 0)
        {
            // Normalize and convert to displayable image
            float min_val = *std::min_element(current_slice_data.begin(), current_slice_data.end());
            float max_val = *std::max_element(current_slice_data.begin(), current_slice_data.end());
            float range = (max_val > min_val) ? (max_val - min_val) : 1.0f;

            cv::Mat img(ny, nx, CV_32F, current_slice_data.data());
            cv::Mat gray8;
            img.convertTo(gray8, CV_8U, 255.0 / range, -255.0 * min_val / range);
            cv::Mat image_rgb;
            cv::cvtColor(gray8, image_rgb, cv::COLOR_GRAY2BGR);

            // Create display with status bar
            const int status_bar_height = 25;
            const int min_width = 180;
            int display_width = std::max(image_rgb.cols, min_width);
            int display_height = image_rgb.rows + status_bar_height;

            cv::Mat display(display_height, display_width, CV_8UC3, cv::Scalar(30, 30, 30));

            int x_offset = (display_width - image_rgb.cols) / 2;
            image_rgb.copyTo(display(cv::Rect(x_offset, 0, image_rgb.cols, image_rgb.rows)));

            // Status bar text
            std::string slice_text = "Slice " + std::to_string(current_slice + 1) + "/" + std::to_string(nz);
            cv::putText(display, slice_text, cv::Point(5, image_rgb.rows + 18),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 1);

            std::string fps_text = std::to_string((int)current_fps) + " fps";
            cv::putText(display, fps_text, cv::Point(display_width - 65, image_rgb.rows + 18),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 255, 0), 1);

            cv::imshow("viz_client", display);
        }
        else
        {
            // No data yet - show waiting message
            cv::Mat waiting = cv::Mat::zeros(240, 320, CV_8UC3);
            cv::putText(waiting, "Waiting for data...", cv::Point(20, 120),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 1);
            cv::imshow("viz_client", waiting);
        }

        // 4. Calculate FPS every second
        auto fps_now = std::chrono::steady_clock::now();
        auto fps_elapsed = std::chrono::duration<double>(fps_now - fps_start).count();
        if (fps_elapsed >= 1.0)
        {
            current_fps = frame_count / fps_elapsed;
            std::cerr << "[FPS DEBUG] Elapsed: " << fps_elapsed << "s, Frames: " << frame_count
                      << ", FPS: " << current_fps << "\n" << std::flush;
            frame_count = 0;
            fps_start = fps_now;
        }

        // 5. Handle keyboard (non-blocking, ~16ms wait for ~60Hz GUI refresh)
        int key = cv::waitKey(16);
        if (key == 27)  // ESC
        {
            std::cout << "viz: ESC pressed, exiting\n";
            break;
        }
        else if (key == 82)  // UP arrow
        {
            int offset = fetcher.get_slice_offset() + 1;
            fetcher.set_slice_offset(offset);
            std::cout << "viz: slice_offset = " << offset << "\n";
        }
        else if (key == 84)  // DOWN arrow
        {
            int offset = fetcher.get_slice_offset() - 1;
            fetcher.set_slice_offset(offset);
            std::cout << "viz: slice_offset = " << offset << "\n";
        }
    }

    cv::destroyAllWindows();
    std::cout << "viz: Done\n";
    return 0;
}
