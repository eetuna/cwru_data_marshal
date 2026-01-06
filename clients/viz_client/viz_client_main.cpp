#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <algorithm>
#include <string>

#include <hdf5.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>

using json = nlohmann::json;

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
// Read single slice from HDF5 file (SWMR mode)
// ============================================================================
static std::vector<float> read_slice(const std::string &path,
                                      uint64_t frame_idx,
                                      int slice_idx,
                                      size_t &nx, size_t &ny, size_t &nz)
{
    // Open file in SWMR read mode
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
    if (fapl < 0)
        return {};

    H5Pset_libver_bounds(fapl, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
    H5Pset_fclose_degree(fapl, H5F_CLOSE_SEMI);

    hid_t file = H5Fopen(path.c_str(), H5F_ACC_RDONLY | H5F_ACC_SWMR_READ, fapl);
    H5Pclose(fapl);

    if (file < 0)
        return {};

    hid_t dset = H5Dopen2(file, "/images/data", H5P_DEFAULT);
    if (dset < 0)
    {
        H5Fclose(file);
        return {};
    }

    // Refresh to see latest data
    H5Drefresh(dset);

    hid_t space = H5Dget_space(dset);
    if (space < 0)
    {
        H5Dclose(dset);
        H5Fclose(file);
        return {};
    }

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
        H5Dclose(dset);
        H5Fclose(file);
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
        H5Dclose(dset);
        H5Fclose(file);
        return {};
    }

    // Create memory space
    hsize_t mem_dims[5] = {1, 1, 1, ny, nx};
    hid_t memspace = H5Screate_simple(5, mem_dims, nullptr);
    if (memspace < 0)
    {
        H5Sclose(space);
        H5Dclose(dset);
        H5Fclose(file);
        return {};
    }

    // Read data
    std::vector<float> buffer(nx * ny);
    herr_t status = H5Dread(dset, H5T_NATIVE_FLOAT, memspace, space, H5P_DEFAULT, buffer.data());

    H5Sclose(memspace);
    H5Sclose(space);
    H5Dclose(dset);
    H5Fclose(file);

    if (status < 0)
        return {};

    return buffer;
}

// ============================================================================
// Main - Simple single-loop design
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

    std::cout << "=== viz_client - Simple Single-Loop ===\n";
    std::cout << "HTTP URL: " << http_url << "\n";
    std::cout << "Controls: UP/DOWN = slice, ESC = exit\n\n";

    // Initialize CURL once (reuse for all requests)
    CURL *curl = curl_easy_init();
    if (!curl)
    {
        std::cerr << "Failed to initialize CURL\n";
        return 1;
    }
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 100L);      // 100ms timeout for fast polling
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 500L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);       // Disable Nagle for lower latency
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);     // Keep connection alive

    cv::namedWindow("viz_client", cv::WINDOW_AUTOSIZE);

    int slice_offset = 0;
    size_t nx = 0, ny = 0, nz = 0;
    uint64_t last_frame = UINT64_MAX;

    // FPS tracking
    int frame_count = 0;
    auto fps_start = std::chrono::steady_clock::now();
    double current_fps = 0.0;

    while (true)
    {
        auto loop_start = std::chrono::steady_clock::now();

        // 1. HTTP poll (reusing CURL handle)
        json j = http_get(curl, http_url);

        if (!j.contains("data") || !j["data"].is_object())
        {
            // No data yet - show waiting message
            cv::Mat waiting = cv::Mat::zeros(240, 320, CV_8UC3);
            cv::putText(waiting, "Waiting for data...", cv::Point(20, 120),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 1);
            cv::imshow("viz_client", waiting);

            int key = cv::waitKey(50);
            if (key == 27) break;
            continue;
        }

        auto data = j["data"];
        if (!data.contains("path") || !data.contains("frame_index"))
        {
            int key = cv::waitKey(50);
            if (key == 27) break;
            continue;
        }

        std::string path = data["path"].get<std::string>();
        uint64_t frame_idx = data["frame_index"].get<uint64_t>();

        // 2. Calculate which slice to read
        int slice = (nz > 0) ? (int)(nz / 2) + slice_offset : 0;
        if (slice < 0) slice = 0;
        if (nz > 0 && (size_t)slice >= nz) slice = nz - 1;

        // 3. Read slice from HDF5
        std::vector<float> slice_data = read_slice(path, frame_idx, slice, nx, ny, nz);

        if (!slice_data.empty())
        {
            // Recalculate slice with actual nz (for first frame)
            if (last_frame == UINT64_MAX)
            {
                slice = (int)(nz / 2) + slice_offset;
                if (slice < 0) slice = 0;
                if ((size_t)slice >= nz) slice = nz - 1;
            }

            // 4. Normalize and convert to displayable image
            float min_val = *std::min_element(slice_data.begin(), slice_data.end());
            float max_val = *std::max_element(slice_data.begin(), slice_data.end());
            float range = (max_val > min_val) ? (max_val - min_val) : 1.0f;

            cv::Mat img(ny, nx, CV_32F, slice_data.data());
            cv::Mat gray8;
            img.convertTo(gray8, CV_8U, 255.0 / range, -255.0 * min_val / range);
            cv::Mat image_rgb;
            cv::cvtColor(gray8, image_rgb, cv::COLOR_GRAY2BGR);

            // Create display with status bar - ensure minimum width for text
            const int status_bar_height = 25;
            const int min_width = 180;  // Minimum width to fit "Slice X/Y" and "XX fps"
            int display_width = std::max(image_rgb.cols, min_width);
            int display_height = image_rgb.rows + status_bar_height;

            cv::Mat display(display_height, display_width, CV_8UC3, cv::Scalar(30, 30, 30));

            // Center the original image (no scaling) in the display area
            int x_offset = (display_width - image_rgb.cols) / 2;
            image_rgb.copyTo(display(cv::Rect(x_offset, 0, image_rgb.cols, image_rgb.rows)));

            // Add text to status bar (below image)
            std::string slice_text = "Slice " + std::to_string(slice + 1) + "/" + std::to_string(nz);
            cv::putText(display, slice_text, cv::Point(5, image_rgb.rows + 18),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 1);

            std::string fps_text = std::to_string((int)current_fps) + " fps";
            cv::putText(display, fps_text, cv::Point(display_width - 65, image_rgb.rows + 18),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 255, 0), 1);

            cv::imshow("viz_client", display);

            // Update FPS counter only on NEW frames
            if (frame_idx != last_frame)
            {
                frame_count++;
                std::cout << "viz: frame " << frame_idx << " slice " << (slice + 1) << "/" << nz << "\n";
                last_frame = frame_idx;
            }

            // Calculate FPS every second
            auto now = std::chrono::steady_clock::now();
            auto fps_elapsed = std::chrono::duration<double>(now - fps_start).count();
            if (fps_elapsed >= 1.0)
            {
                current_fps = frame_count / fps_elapsed;
                frame_count = 0;
                fps_start = now;
            }
        }

        // 5. Handle keyboard (non-blocking with short wait)
        int key = cv::waitKey(1);
        if (key == 27)  // ESC
        {
            std::cout << "viz: ESC pressed, exiting\n";
            break;
        }
        else if (key == 82)  // UP arrow
        {
            slice_offset++;
            std::cout << "viz: slice_offset = " << slice_offset << "\n";
        }
        else if (key == 84)  // DOWN arrow
        {
            slice_offset--;
            std::cout << "viz: slice_offset = " << slice_offset << "\n";
        }

        // 6. Small sleep to prevent CPU spinning when no new data
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    curl_easy_cleanup(curl);
    cv::destroyAllWindows();
    std::cout << "viz: Done\n";
    return 0;
}
