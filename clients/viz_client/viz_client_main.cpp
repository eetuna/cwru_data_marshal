/*
 * File: clients/viz_client/viz_client_main.cpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Poll GET /image/latest, open standalone file, render with OpenCV
 *
 * If "error" is false: read file as raw ISMRMRD image wire bytes
 *   (198B ImageHeader + uint64 attr_len + attr + pixels)
 * If "error" is true: read file as PNG via cv::imread
 *
 * No SWMR. No /stream endpoint. No network image delivery.
 */

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

int main(int argc, char** argv) {
    Options opt = parse_args(argc, argv);
    std::cout << "viz_client: polling " << opt.marshal_url << "/image/latest every "
              << opt.poll_ms << "ms\n";

    CURL* curl = curl_easy_init();
    if (!curl) { std::cerr << "curl init failed\n"; return 1; }

    std::string last_path;
    cv::namedWindow("MRI Viz", cv::WINDOW_NORMAL);

    for (;;) {
        std::string resp = http_get(curl, opt.marshal_url + "/image/latest");
        if (resp.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(opt.poll_ms));
            continue;
        }

        try {
            auto j = nlohmann::json::parse(resp);
            std::string path = j.value("path", "");
            bool is_error = j.value("error", false);

            if (path.empty() || path == last_path) {
                std::this_thread::sleep_for(std::chrono::milliseconds(opt.poll_ms));
                int key = cv::waitKey(1);
                if (key == 27) break;
                continue;
            }
            last_path = path;

            if (is_error) {
                cv::Mat img = cv::imread(path, cv::IMREAD_COLOR);
                if (!img.empty())
                    cv::imshow("MRI Viz", img);
            } else {
                std::vector<uint8_t> data;
                if (!read_file(path, data) || data.size() < sizeof(ISMRMRD::ImageHeader) + sizeof(uint64_t)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(opt.poll_ms));
                    continue;
                }

                const auto* hdr = reinterpret_cast<const ISMRMRD::ImageHeader*>(data.data());
                uint64_t attr_len = 0;
                std::memcpy(&attr_len, data.data() + sizeof(ISMRMRD::ImageHeader), sizeof(uint64_t));
                size_t pixel_offset = sizeof(ISMRMRD::ImageHeader) + sizeof(uint64_t) + attr_len;

                if (pixel_offset >= data.size()) continue;

                uint16_t nx = hdr->matrix_size[0];
                uint16_t ny = hdr->matrix_size[1];
                if (nx == 0 || ny == 0) continue;

                const float* pixels = reinterpret_cast<const float*>(data.data() + pixel_offset);
                size_t npixels = (data.size() - pixel_offset) / sizeof(float);
                if (npixels < static_cast<size_t>(nx) * ny) continue;

                cv::Mat slice(ny, nx, CV_32FC1, const_cast<float*>(pixels));
                cv::Mat display;
                cv::normalize(slice, display, 0, 255, cv::NORM_MINMAX, CV_8UC1);
                cv::Mat color;
                cv::applyColorMap(display, color, cv::COLORMAP_BONE);
                cv::imshow("MRI Viz", color);
            }
        } catch (...) {}

        int key = cv::waitKey(1);
        if (key == 27) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(opt.poll_ms));
    }

    curl_easy_cleanup(curl);
    cv::destroyAllWindows();
    return 0;
}
