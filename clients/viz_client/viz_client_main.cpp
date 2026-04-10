/*
 * viz_client — polls GET /image/latest, reads standalone file, renders with OpenCV
 *
 * Features: FPS counter, slice info overlay, auto-updates on new frames.
 * Controls: ESC to quit.
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
    int poll_ms{50};
};

Options parse_args(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--http" && i + 1 < argc) opt.marshal_url = argv[++i];
        else if (arg == "--poll-ms" && i + 1 < argc) opt.poll_ms = std::stoi(argv[++i]);
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

int main(int argc, char** argv) {
    Options opt = parse_args(argc, argv);
    std::cout << "viz_client: polling " << opt.marshal_url << "/image/latest\n";

    CURL* curl = curl_easy_init();
    if (!curl) { std::cerr << "curl init failed\n"; return 1; }

    cv::namedWindow("MRI Viz", cv::WINDOW_NORMAL);

    size_t frame_count = 0;
    auto fps_start = std::chrono::steady_clock::now();
    double fps = 0.0;
    auto last_print = std::chrono::steady_clock::now();

    for (;;) {
        auto resp = http_get(curl, opt.marshal_url + "/image/latest");
        if (resp.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(opt.poll_ms));
            if (cv::waitKey(1) == 27) break;
            continue;
        }

        try {
            auto j = nlohmann::json::parse(resp);
            std::string path = j.value("path", "");
            bool is_error = j.value("error", false);

            if (path.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(opt.poll_ms));
                if (cv::waitKey(1) == 27) break;
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
                std::vector<uint8_t> data;
                if (!read_file(path, data) ||
                    data.size() < sizeof(ISMRMRD::ImageHeader) + sizeof(uint64_t)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(opt.poll_ms));
                    if (cv::waitKey(1) == 27) break;
                    continue;
                }

                const auto* hdr = reinterpret_cast<const ISMRMRD::ImageHeader*>(data.data());
                uint64_t attr_len = 0;
                std::memcpy(&attr_len, data.data() + sizeof(ISMRMRD::ImageHeader), sizeof(uint64_t));
                size_t pixel_offset = sizeof(ISMRMRD::ImageHeader) + sizeof(uint64_t) + attr_len;

                uint16_t nx = hdr->matrix_size[0];
                uint16_t ny = hdr->matrix_size[1];
                if (nx == 0 || ny == 0 || pixel_offset >= data.size()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(opt.poll_ms));
                    if (cv::waitKey(1) == 27) break;
                    continue;
                }

                const float* pixels = reinterpret_cast<const float*>(data.data() + pixel_offset);
                size_t npixels = (data.size() - pixel_offset) / sizeof(float);
                if (npixels < static_cast<size_t>(nx) * ny) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(opt.poll_ms));
                    if (cv::waitKey(1) == 27) break;
                    continue;
                }

                frame_count++;

                // FPS
                auto now = std::chrono::steady_clock::now();
                double elapsed = std::chrono::duration<double>(now - fps_start).count();
                if (elapsed >= 1.0) {
                    fps = frame_count / elapsed;
                    frame_count = 0;
                    fps_start = now;
                }

                // Render
                cv::Mat slice(ny, nx, CV_32FC1, const_cast<float*>(pixels));
                cv::Mat display;
                cv::normalize(slice, display, 0, 255, cv::NORM_MINMAX, CV_8UC1);
                cv::Mat color;
                cv::applyColorMap(display, color, cv::COLORMAP_BONE);

                // Overlay
                std::ostringstream oss;
                oss << "FPS: " << static_cast<int>(fps)
                    << " | " << nx << "x" << ny
                    << " | slice=" << hdr->slice
                    << " series=" << hdr->image_series_index;
                cv::putText(color, oss.str(), {10, 25},
                            cv::FONT_HERSHEY_SIMPLEX, 0.5, {0, 255, 0}, 1);
                cv::imshow("MRI Viz", color);

                // Terminal output every second
                if (std::chrono::duration<double>(now - last_print).count() >= 1.0) {
                    std::cout << "[FPS: " << static_cast<int>(fps)
                              << "] " << nx << "x" << ny
                              << " slice=" << hdr->slice
                              << " series=" << hdr->image_series_index << "\n";
                    last_print = now;
                }
            }
        } catch (...) {}

        if (cv::waitKey(1) == 27) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(opt.poll_ms));
    }

    curl_easy_cleanup(curl);
    cv::destroyAllWindows();
    return 0;
}
