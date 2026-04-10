/*
 * File: clients/viz_client/viz_client_main.cpp
 * Purpose: Poll GET /image/latest, read multi-slice standalone file, render with OpenCV
 *
 * Features:
 *   - FPS counter (displayed on image + terminal)
 *   - Slice number display (e.g. "Slice 2/5")
 *   - Arrow key slice scrolling (UP/DOWN or LEFT/RIGHT)
 *   - Auto-updates when new images arrive
 *
 * File format: latest_image.bin contains concatenated records:
 *   [4B LE image_size][image_wire_bytes] repeated per slice
 *   where image_wire_bytes = 198B ImageHeader + 8B attr_len + attr + pixels
 */

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <ismrmrd/ismrmrd.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>

struct Options {
    std::string marshal_url{"http://localhost:8080"};
    int poll_ms{100};
};

Options parse_args(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--http" && i + 1 < argc)
            opt.marshal_url = argv[++i];
        else if (arg == "--poll-ms" && i + 1 < argc)
            opt.poll_ms = std::stoi(argv[++i]);
    }
    return opt;
}

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

// Parse one image from wire bytes at offset. Returns pixel Mat or empty.
struct SliceInfo {
    cv::Mat pixels;
    uint16_t slice_idx;
    uint16_t series_idx;
    uint16_t nx, ny;
};

SliceInfo parse_image(const uint8_t* data, size_t size) {
    SliceInfo info{};
    if (size < sizeof(ISMRMRD::ImageHeader) + sizeof(uint64_t)) return info;

    const auto* hdr = reinterpret_cast<const ISMRMRD::ImageHeader*>(data);
    uint64_t attr_len = 0;
    std::memcpy(&attr_len, data + sizeof(ISMRMRD::ImageHeader), sizeof(uint64_t));
    size_t pixel_offset = sizeof(ISMRMRD::ImageHeader) + sizeof(uint64_t) + attr_len;
    if (pixel_offset >= size) return info;

    info.nx = hdr->matrix_size[0];
    info.ny = hdr->matrix_size[1];
    info.slice_idx = hdr->slice;
    info.series_idx = hdr->image_series_index;
    if (info.nx == 0 || info.ny == 0) return info;

    const float* px = reinterpret_cast<const float*>(data + pixel_offset);
    size_t npixels = (size - pixel_offset) / sizeof(float);
    if (npixels < static_cast<size_t>(info.nx) * info.ny) return info;

    info.pixels = cv::Mat(info.ny, info.nx, CV_32FC1, const_cast<float*>(px)).clone();
    return info;
}

// Parse all slices from the multi-record file
std::vector<SliceInfo> parse_all_slices(const std::vector<uint8_t>& data) {
    std::vector<SliceInfo> slices;
    size_t offset = 0;
    while (offset + 4 <= data.size()) {
        uint32_t img_size = 0;
        std::memcpy(&img_size, data.data() + offset, 4);
        offset += 4;
        if (offset + img_size > data.size()) break;

        auto info = parse_image(data.data() + offset, img_size);
        if (!info.pixels.empty())
            slices.push_back(std::move(info));
        offset += img_size;
    }
    return slices;
}

int main(int argc, char** argv) {
    Options opt = parse_args(argc, argv);
    std::cout << "viz_client: polling " << opt.marshal_url << "/image/latest\n"
              << "  Controls: UP/DOWN or LEFT/RIGHT = scroll slices, ESC = quit\n";

    CURL* curl = curl_easy_init();
    if (!curl) { std::cerr << "curl init failed\n"; return 1; }

    cv::namedWindow("MRI Viz", cv::WINDOW_NORMAL);

    int current_slice = 0;
    int total_slices = 0;
    uint32_t last_count = 0;
    size_t last_file_size = 0;
    size_t frame_count = 0;
    auto fps_start = std::chrono::steady_clock::now();
    double fps = 0.0;

    for (;;) {
        std::string resp = http_get(curl, opt.marshal_url + "/image/latest");
        if (resp.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(opt.poll_ms));
            int key = cv::waitKey(1);
            if (key == 27) break;
            continue;
        }

        try {
            auto j = nlohmann::json::parse(resp);
            std::string path = j.value("path", "");
            bool is_error = j.value("error", false);
            uint32_t server_count = j.value("slices", 0u);

            if (path.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(opt.poll_ms));
                int key = cv::waitKey(1);
                if (key == 27) break;
                continue;
            }

            if (is_error) {
                cv::Mat img = cv::imread(path, cv::IMREAD_COLOR);
                if (!img.empty()) {
                    cv::putText(img, "RECON FAILED", {10, 30},
                                cv::FONT_HERSHEY_SIMPLEX, 0.8, {0, 0, 255}, 2);
                    cv::imshow("MRI Viz", img);
                }
            } else {
                // Read the multi-record file
                std::vector<uint8_t> data;
                if (!read_file(path, data) || data.size() < 4) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(opt.poll_ms));
                    int key = cv::waitKey(1);
                    if (key == 27) break;
                    continue;
                }

                // Only reparse if file changed
                bool changed = (data.size() != last_file_size) || (server_count != last_count);
                if (changed) {
                    auto slices = parse_all_slices(data);
                    total_slices = static_cast<int>(slices.size());
                    last_file_size = data.size();
                    last_count = server_count;

                    if (current_slice >= total_slices && total_slices > 0)
                        current_slice = total_slices - 1;

                    if (total_slices > 0) {
                        frame_count++;

                        // FPS calculation
                        auto now = std::chrono::steady_clock::now();
                        double elapsed = std::chrono::duration<double>(now - fps_start).count();
                        if (elapsed >= 1.0) {
                            fps = frame_count / elapsed;
                            frame_count = 0;
                            fps_start = now;
                        }

                        // Display selected slice
                        const auto& s = slices[current_slice];
                        cv::Mat display;
                        cv::normalize(s.pixels, display, 0, 255, cv::NORM_MINMAX, CV_8UC1);
                        cv::Mat color;
                        cv::applyColorMap(display, color, cv::COLORMAP_BONE);

                        // Overlay text: FPS + slice info
                        std::ostringstream oss;
                        oss << "FPS: " << static_cast<int>(fps)
                            << " | Slice " << (current_slice + 1) << "/" << total_slices
                            << " (" << s.nx << "x" << s.ny << ")"
                            << " series=" << s.series_idx;
                        cv::putText(color, oss.str(), {10, 25},
                                    cv::FONT_HERSHEY_SIMPLEX, 0.6, {0, 255, 0}, 1);

                        cv::imshow("MRI Viz", color);

                        // Terminal FPS
                        if (static_cast<int>(frame_count) % 10 == 0) {
                            std::cout << "[FPS: " << static_cast<int>(fps)
                                      << "] Slice " << (current_slice + 1) << "/" << total_slices
                                      << " (" << s.nx << "x" << s.ny << ")\n";
                        }
                    }
                }
            }
        } catch (...) {}

        // Key handling: arrow keys for slice scrolling
        int key = cv::waitKey(1);
        if (key == 27) break;                    // ESC
        if ((key == 82 || key == 0) && total_slices > 0) {  // UP arrow
            current_slice = (current_slice + 1) % total_slices;
            last_file_size = 0;  // force redraw
        }
        if ((key == 84 || key == 1) && total_slices > 0) {  // DOWN arrow
            current_slice = (current_slice - 1 + total_slices) % total_slices;
            last_file_size = 0;  // force redraw
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(opt.poll_ms));
    }

    curl_easy_cleanup(curl);
    cv::destroyAllWindows();
    return 0;
}
