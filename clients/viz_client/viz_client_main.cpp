/*
 * viz_client — polls GET /image/latest, reads multi-slice standalone file, renders with OpenCV
 *
 * File format (written by marshal's handle_recon_image):
 *   uint16_t nz  — number of spatial slices in this volume
 *   nz × (198B ImageHeader + 8B attr_len + attr_bytes + pixel_bytes)
 *
 * Each image is standard ISMRMRD wire format (same as connection.py send_image).
 * The ImageHeader.slice field identifies the spatial slice position.
 * The ImageHeader.data_type field determines pixel size.
 *
 * UP/DOWN arrows scroll through spatial slices (like a DICOM viewer).
 * Selection persists across time updates.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
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

#include "mrd_stream_tags.hpp"

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

// Compute bytes per pixel from ISMRMRD data_type enum
static size_t itemsize_for_data_type(uint16_t dt) {
    switch (dt) {
        case ISMRMRD::ISMRMRD_USHORT:   return 2;
        case ISMRMRD::ISMRMRD_SHORT:    return 2;
        case ISMRMRD::ISMRMRD_UINT:     return 4;
        case ISMRMRD::ISMRMRD_INT:      return 4;
        case ISMRMRD::ISMRMRD_FLOAT:    return 4;
        case ISMRMRD::ISMRMRD_DOUBLE:   return 8;
        case ISMRMRD::ISMRMRD_CXFLOAT:  return 8;
        case ISMRMRD::ISMRMRD_CXDOUBLE: return 16;
        default: return 4;
    }
}

// Extract float pixels from any ISMRMRD data type (magnitude for complex)
static void extract_float_pixels(const void* src, size_t npixels, uint16_t data_type,
                                  std::vector<float>& out) {
    out.resize(npixels);
    switch (data_type) {
    case ISMRMRD::ISMRMRD_FLOAT:
        std::memcpy(out.data(), src, npixels * sizeof(float));
        break;
    case ISMRMRD::ISMRMRD_SHORT: {
        auto* p = static_cast<const int16_t*>(src);
        for (size_t i = 0; i < npixels; ++i) out[i] = static_cast<float>(p[i]);
        break;
    }
    case ISMRMRD::ISMRMRD_USHORT: {
        auto* p = static_cast<const uint16_t*>(src);
        for (size_t i = 0; i < npixels; ++i) out[i] = static_cast<float>(p[i]);
        break;
    }
    case ISMRMRD::ISMRMRD_INT: {
        auto* p = static_cast<const int32_t*>(src);
        for (size_t i = 0; i < npixels; ++i) out[i] = static_cast<float>(p[i]);
        break;
    }
    case ISMRMRD::ISMRMRD_UINT: {
        auto* p = static_cast<const uint32_t*>(src);
        for (size_t i = 0; i < npixels; ++i) out[i] = static_cast<float>(p[i]);
        break;
    }
    case ISMRMRD::ISMRMRD_DOUBLE: {
        auto* p = static_cast<const double*>(src);
        for (size_t i = 0; i < npixels; ++i) out[i] = static_cast<float>(p[i]);
        break;
    }
    case ISMRMRD::ISMRMRD_CXFLOAT: {
        // Magnitude of complex float
        auto* p = static_cast<const float*>(src);
        for (size_t i = 0; i < npixels; ++i) {
            float re = p[2*i], im = p[2*i+1];
            out[i] = std::sqrt(re*re + im*im);
        }
        break;
    }
    case ISMRMRD::ISMRMRD_CXDOUBLE: {
        auto* p = static_cast<const double*>(src);
        for (size_t i = 0; i < npixels; ++i) {
            double re = p[2*i], im = p[2*i+1];
            out[i] = static_cast<float>(std::sqrt(re*re + im*im));
        }
        break;
    }
    default:
        std::memset(out.data(), 0, npixels * sizeof(float));
        break;
    }
}

struct SliceImage {
    uint16_t nx{0}, ny{0};
    uint16_t slice_idx{0};
    std::vector<float> pixels;
};

// Parse multi-slice file: uint16_t nz + nz × wire-format images
static bool parse_multislice_file(const std::vector<uint8_t>& data,
                                   std::vector<SliceImage>& slices_out) {
    slices_out.clear();
    if (data.size() < sizeof(uint16_t)) return false;

    uint16_t nz = 0;
    std::memcpy(&nz, data.data(), sizeof(uint16_t));
    if (nz == 0 || nz > 256) return false;

    size_t off = sizeof(uint16_t);
    for (uint16_t i = 0; i < nz; ++i) {
        if (off + mrd::IMAGE_HEADER_BYTES + sizeof(uint64_t) > data.size())
            return false;

        const auto* hdr = reinterpret_cast<const ISMRMRD::ImageHeader*>(data.data() + off);
        uint64_t attr_len = 0;
        std::memcpy(&attr_len, data.data() + off + mrd::IMAGE_HEADER_BYTES, sizeof(uint64_t));

        size_t pixel_offset = off + mrd::IMAGE_HEADER_BYTES + sizeof(uint64_t) + attr_len;
        uint16_t w = hdr->matrix_size[0];
        uint16_t h = hdr->matrix_size[1];
        uint16_t z = std::max<uint16_t>(hdr->matrix_size[2], 1);
        uint16_t ch = std::max<uint16_t>(hdr->channels, 1);
        size_t npixels = static_cast<size_t>(w) * h * z * ch;
        size_t is = itemsize_for_data_type(hdr->data_type);
        size_t pixel_bytes = npixels * is;

        if (pixel_offset + pixel_bytes > data.size())
            return false;

        SliceImage si;
        si.nx = w;
        si.ny = h;
        si.slice_idx = hdr->slice;
        // For multi-channel: use first channel only (w*h pixels)
        size_t display_pixels = static_cast<size_t>(w) * h;
        extract_float_pixels(data.data() + pixel_offset, display_pixels,
                             hdr->data_type, si.pixels);
        slices_out.push_back(std::move(si));

        // Advance past this image
        off = pixel_offset + pixel_bytes;
    }
    return !slices_out.empty();
}

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    std::string http_url = "http://localhost:8080";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--http" || arg == "-http") && i + 1 < argc)
            http_url = argv[++i];
    }

    std::cout << "=== viz_client ===\n";
    std::cout << "HTTP URL: " << http_url << "\n";
    std::cout << "Controls: UP/DOWN = scroll spatial slices, ESC = exit\n\n";

    CURL* curl = curl_easy_init();
    if (!curl) { std::cerr << "curl init failed\n"; return 1; }

    cv::namedWindow("viz_client", cv::WINDOW_AUTOSIZE);

    // Current volume (all spatial slices)
    std::vector<SliceImage> current_volume;
    int selected_slice = 0;  // 0-indexed, persists across time updates

    // Change detection via file modification time (not counters or file size)
    std::string last_path;
    std::filesystem::file_time_type last_mtime{};

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

                if (!path.empty() && !is_error) {
                    // Detect changes via file mtime (atomic rename changes mtime reliably)
                    std::error_code ec;
                    auto mtime = std::filesystem::last_write_time(path, ec);
                    if (!ec && (path != last_path || mtime != last_mtime)) {
                        std::vector<uint8_t> data;
                        if (read_file(path, data) && data.size() > sizeof(uint16_t)) {
                            std::vector<SliceImage> new_volume;
                            if (parse_multislice_file(data, new_volume)) {
                                current_volume = std::move(new_volume);
                                // Update tracking ONLY after successful parse
                                last_path = path;
                                last_mtime = mtime;

                                int nz = static_cast<int>(current_volume.size());
                                if (selected_slice >= nz) selected_slice = nz - 1;
                                if (selected_slice < 0) selected_slice = 0;

                                frame_count++;
                                total_frames++;

                                auto& s0 = current_volume[selected_slice];
                                std::cout << "viz: frame " << total_frames
                                          << " nz=" << current_volume.size()
                                          << " viewing slice " << (selected_slice + 1)
                                          << "/" << current_volume.size()
                                          << " (" << s0.nx << "x" << s0.ny << ")\n";
                            }
                        }
                    }
                } else if (is_error && !path.empty()) {
                    cv::Mat err_img = cv::imread(path, cv::IMREAD_COLOR);
                    if (!err_img.empty()) {
                        cv::putText(err_img, "RECON FAILED", {10, 30},
                                    cv::FONT_HERSHEY_SIMPLEX, 0.8, {0, 0, 255}, 2);
                        cv::imshow("viz_client", err_img);
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "viz error: " << e.what() << "\n";
            }
        }

        // 2. Render selected spatial slice
        if (!current_volume.empty() && selected_slice < static_cast<int>(current_volume.size())) {
            const auto& si = current_volume[selected_slice];
            if (!si.pixels.empty() && si.nx > 0 && si.ny > 0) {
                float min_val = *std::min_element(si.pixels.begin(), si.pixels.end());
                float max_val = *std::max_element(si.pixels.begin(), si.pixels.end());
                float range = (max_val > min_val) ? (max_val - min_val) : 1.0f;

                cv::Mat img(si.ny, si.nx, CV_32F, const_cast<float*>(si.pixels.data()));
                cv::Mat gray8;
                img.convertTo(gray8, CV_8U, 255.0 / range, -255.0 * min_val / range);
                cv::Mat image_rgb;
                cv::cvtColor(gray8, image_rgb, cv::COLOR_GRAY2BGR);

                // Status bar
                const int bar_h = 25;
                const int min_w = 200;
                int dw = std::max(image_rgb.cols, min_w);
                int dh = image_rgb.rows + bar_h;

                cv::Mat display(dh, dw, CV_8UC3, cv::Scalar(30, 30, 30));
                int xoff = (dw - image_rgb.cols) / 2;
                image_rgb.copyTo(display(cv::Rect(xoff, 0, image_rgb.cols, image_rgb.rows)));

                // Slice info (left)
                int nz = static_cast<int>(current_volume.size());
                std::string slice_text = "Slice " + std::to_string(selected_slice + 1) +
                                         "/" + std::to_string(nz);
                cv::putText(display, slice_text, cv::Point(5, image_rgb.rows + 18),
                            cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(0, 255, 0), 1);

                // FPS (right)
                std::string fps_text = std::to_string(static_cast<int>(current_fps)) + " fps";
                cv::putText(display, fps_text, cv::Point(dw - 55, image_rgb.rows + 18),
                            cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(255, 255, 0), 1);

                cv::imshow("viz_client", display);
            }
        } else {
            cv::Mat waiting = cv::Mat::zeros(240, 320, CV_8UC3);
            cv::putText(waiting, "Waiting for data...", cv::Point(20, 120),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 1);
            cv::imshow("viz_client", waiting);
        }

        // 3. FPS calculation every second
        auto fps_now = std::chrono::steady_clock::now();
        double fps_elapsed = std::chrono::duration<double>(fps_now - fps_start).count();
        if (fps_elapsed >= 1.0) {
            current_fps = frame_count / fps_elapsed;
            std::cerr << "[FPS DEBUG] Elapsed: " << fps_elapsed << "s, Frames: " << frame_count
                      << ", FPS: " << current_fps << "\n";
            frame_count = 0;
            fps_start = fps_now;
        }

        // 4. Key handling (16ms wait = ~60Hz GUI refresh)
        int key = cv::waitKey(16);
        if (key == 27) {
            std::cout << "viz: ESC pressed, exiting\n";
            break;
        } else if (key == 82) {  // UP arrow
            int nz = static_cast<int>(current_volume.size());
            selected_slice = std::min(selected_slice + 1, std::max(nz - 1, 0));
            std::cout << "viz: selected_slice = " << (selected_slice + 1) << "/" << nz << "\n";
        } else if (key == 84) {  // DOWN arrow
            selected_slice = std::max(selected_slice - 1, 0);
            int nz = static_cast<int>(current_volume.size());
            std::cout << "viz: selected_slice = " << (selected_slice + 1) << "/" << nz << "\n";
        }
    }

    curl_easy_cleanup(curl);
    cv::destroyAllWindows();
    std::cout << "viz: Done\n";
    return 0;
}
