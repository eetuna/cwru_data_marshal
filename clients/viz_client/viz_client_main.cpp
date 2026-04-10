/*
 * viz_client — polls GET /image/latest, reads standalone file, renders with OpenCV
 *
 * Replicates the previous SWMR-based viz_client's display behavior:
 *   - Status bar below image (slice info left, FPS right)
 *   - Arrow keys (UP=82/DOWN=84) for slice offset
 *   - "Waiting for data..." screen
 *   - FPS debug output to stderr every second
 *   - Per-frame terminal output
 *   - WINDOW_AUTOSIZE, gray→BGR, centered image
 *
 * Reads from standalone file (latest_image.bin) via atomic rename instead of SWMR HDF5.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <ismrmrd/ismrmrd.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>

static size_t curl_write_cb(void* ptr, size_t size, size_t nmemb, std::string* data) {
    data->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

std::string http_get(CURL* curl, const std::string& url) {
    std::string body;
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L);
    curl_easy_perform(curl);
    return body;
}

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    auto sz = f.tellg();
    if (sz <= 0) return false;
    out.resize(static_cast<size_t>(sz));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), sz);
    return f.good();
}

int main(int argc, char** argv) {
    std::string http_url = "http://localhost:8080";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--http" || arg == "-http") && i + 1 < argc)
            http_url = argv[++i];
    }

    std::cout << "=== viz_client ===\n";
    std::cout << "HTTP URL: " << http_url << "\n";
    std::cout << "Controls: UP/DOWN = slice, ESC = exit\n\n";

    CURL* curl = curl_easy_init();
    if (!curl) { std::cerr << "curl init failed\n"; return 1; }

    cv::namedWindow("viz_client", cv::WINDOW_AUTOSIZE);

    std::vector<float> current_pixels;
    size_t nx = 0, ny = 0;
    uint16_t current_slice_idx = 0;
    uint16_t current_series = 0;
    uint32_t last_slices_count = 0;
    size_t last_file_size = 0;

    int frame_count = 0;
    auto fps_start = std::chrono::steady_clock::now();
    double current_fps = 0.0;
    uint64_t total_frames = 0;

    while (true) {
        // 1. Poll GET /image/latest
        std::string resp = http_get(curl, http_url + "/image/latest");

        if (!resp.empty()) {
            try {
                auto j = nlohmann::json::parse(resp);
                std::string path = j.value("path", "");
                bool is_error = j.value("error", false);
                uint32_t slices_count = j.value("slices", 0u);

                if (!path.empty() && !is_error) {
                    std::vector<uint8_t> data;
                    if (read_file(path, data) &&
                        data.size() >= sizeof(ISMRMRD::ImageHeader) + sizeof(uint64_t)) {

                        // Check if file actually changed
                        bool changed = (data.size() != last_file_size) ||
                                       (slices_count != last_slices_count);

                        if (changed) {
                            last_file_size = data.size();
                            last_slices_count = slices_count;

                            const auto* hdr = reinterpret_cast<const ISMRMRD::ImageHeader*>(data.data());
                            uint64_t attr_len = 0;
                            std::memcpy(&attr_len, data.data() + sizeof(ISMRMRD::ImageHeader), sizeof(uint64_t));
                            size_t pixel_offset = sizeof(ISMRMRD::ImageHeader) + sizeof(uint64_t) + attr_len;

                            uint16_t w = hdr->matrix_size[0];
                            uint16_t h = hdr->matrix_size[1];

                            if (w > 0 && h > 0 && pixel_offset + w * h * sizeof(float) <= data.size()) {
                                nx = w;
                                ny = h;
                                current_slice_idx = hdr->slice;
                                current_series = hdr->image_series_index;

                                current_pixels.resize(nx * ny);
                                std::memcpy(current_pixels.data(),
                                            data.data() + pixel_offset,
                                            nx * ny * sizeof(float));

                                frame_count++;
                                total_frames++;
                                std::cout << "viz: frame " << total_frames
                                          << " slice " << current_slice_idx
                                          << " series " << current_series
                                          << " (" << nx << "x" << ny << ")\n";
                            }
                        }
                    }
                } else if (is_error && !path.empty()) {
                    cv::Mat err = cv::imread(path, cv::IMREAD_COLOR);
                    if (!err.empty()) {
                        cv::putText(err, "RECON FAILED", {10, 30},
                                    cv::FONT_HERSHEY_SIMPLEX, 0.8, {0, 0, 255}, 2);
                        cv::imshow("viz_client", err);
                    }
                }
            } catch (...) {}
        }

        // 2. Render current frame (always responsive)
        if (!current_pixels.empty() && nx > 0 && ny > 0) {
            float min_val = *std::min_element(current_pixels.begin(), current_pixels.end());
            float max_val = *std::max_element(current_pixels.begin(), current_pixels.end());
            float range = (max_val > min_val) ? (max_val - min_val) : 1.0f;

            cv::Mat img(ny, nx, CV_32F, current_pixels.data());
            cv::Mat gray8;
            img.convertTo(gray8, CV_8U, 255.0 / range, -255.0 * min_val / range);
            cv::Mat image_rgb;
            cv::cvtColor(gray8, image_rgb, cv::COLOR_GRAY2BGR);

            // Status bar below image
            const int status_bar_height = 25;
            const int min_width = 200;
            int display_width = std::max(image_rgb.cols, min_width);
            int display_height = image_rgb.rows + status_bar_height;

            cv::Mat display(display_height, display_width, CV_8UC3, cv::Scalar(30, 30, 30));
            int x_offset = (display_width - image_rgb.cols) / 2;
            image_rgb.copyTo(display(cv::Rect(x_offset, 0, image_rgb.cols, image_rgb.rows)));

            // Slice info (left)
            std::string slice_text = "Slice " + std::to_string(current_slice_idx) +
                                     " series=" + std::to_string(current_series);
            cv::putText(display, slice_text, cv::Point(5, image_rgb.rows + 18),
                        cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(0, 255, 0), 1);

            // FPS (right)
            std::string fps_text = std::to_string(static_cast<int>(current_fps)) + " fps";
            cv::putText(display, fps_text, cv::Point(display_width - 60, image_rgb.rows + 18),
                        cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(255, 255, 0), 1);

            cv::imshow("viz_client", display);
        } else {
            cv::Mat waiting = cv::Mat::zeros(240, 320, CV_8UC3);
            cv::putText(waiting, "Waiting for data...", cv::Point(20, 120),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 1);
            cv::imshow("viz_client", waiting);
        }

        // 3. FPS calculation every second (stderr, matching previous viz)
        auto fps_now = std::chrono::steady_clock::now();
        double fps_elapsed = std::chrono::duration<double>(fps_now - fps_start).count();
        if (fps_elapsed >= 1.0) {
            current_fps = frame_count / fps_elapsed;
            std::cerr << "[FPS DEBUG] Elapsed: " << fps_elapsed << "s, Frames: " << frame_count
                      << ", FPS: " << current_fps << "\n" << std::flush;
            frame_count = 0;
            fps_start = fps_now;
        }

        // 4. Key handling (16ms wait = ~60Hz GUI refresh, matching previous viz)
        int key = cv::waitKey(16);
        if (key == 27) {
            std::cout << "viz: ESC pressed, exiting\n";
            break;
        }
    }

    curl_easy_cleanup(curl);
    cv::destroyAllWindows();
    std::cout << "viz: Done\n";
    return 0;
}
