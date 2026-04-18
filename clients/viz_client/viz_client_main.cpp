/*
 * viz_client — polls GET /image/latest, reads canonical ISMRMRD H5, renders with OpenCV.
 *
 * The marshal writes latest_image.h5 using libismrmrd Dataset::appendImage,
 * matching python-ismrmrd-server's saved-image pattern. Images for the latest
 * live view are stored in group image_0.
 *
 * UP/DOWN arrows scroll through spatial slices (like a DICOM viewer).
 * Selection persists across time updates.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <hdf5.h>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <ismrmrd/ismrmrd.h>
#include <ismrmrd/dataset.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>

#include "mrd_stream_tags.hpp"

static size_t curl_write_cb(void* ptr, size_t size, size_t nmemb, std::string* data) {
    data->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

struct HttpGetResult {
    bool ok{false};
    long status_code{0};
    std::string body;
};

static HttpGetResult http_get(CURL* curl, const std::string& url) {
    HttpGetResult result;
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const auto rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        std::cerr << "viz http error: " << curl_easy_strerror(rc) << "\n";
        return result;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status_code);
    result.ok = (result.status_code >= 200 && result.status_code < 300)
             || result.status_code == 204;
    if (!result.ok) {
        std::cerr << "viz http status error: " << result.status_code << "\n";
    }
    return result;
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

struct ScopedH5Handle {
    using Closer = herr_t (*)(hid_t);

    hid_t id{-1};
    Closer closer{nullptr};

    ScopedH5Handle() = default;
    ScopedH5Handle(hid_t handle, Closer close_fn)
        : id(handle), closer(close_fn)
    {}

    ScopedH5Handle(const ScopedH5Handle&) = delete;
    ScopedH5Handle& operator=(const ScopedH5Handle&) = delete;

    ~ScopedH5Handle()
    {
        if (id >= 0 && closer) closer(id);
    }

    explicit operator bool() const noexcept { return id >= 0; }
};

struct SliceImage {
    uint16_t nx{0}, ny{0};
    uint16_t slice_idx{0};
    std::vector<float> pixels;
};

template <typename T>
static void append_image_slices(const ISMRMRD::Image<T>& img,
                                std::vector<SliceImage>& slices_out) {
    const auto& hdr = img.getHead();
    uint16_t w = hdr.matrix_size[0];
    uint16_t h = hdr.matrix_size[1];
    uint16_t d = std::max<uint16_t>(1, hdr.matrix_size[2]);
    if (w == 0 || h == 0) return;

    std::vector<float> all_pixels;
    const size_t total_pixels = static_cast<size_t>(w) * h * d;
    extract_float_pixels(img.getDataPtr(), total_pixels, hdr.data_type, all_pixels);

    const size_t slice_pixels = static_cast<size_t>(w) * h;
    for (uint16_t z = 0; z < d; ++z) {
        SliceImage si;
        si.nx = w;
        si.ny = h;
        si.slice_idx = d > 1 ? z : hdr.slice;
        auto begin = all_pixels.begin() + static_cast<std::ptrdiff_t>(z * slice_pixels);
        auto end = begin + static_cast<std::ptrdiff_t>(slice_pixels);
        si.pixels.assign(begin, end);
        slices_out.push_back(std::move(si));
    }
}

template <typename T>
static bool read_dataset_image_typed(ISMRMRD::Dataset& ds,
                                     uint32_t index,
                                     std::vector<SliceImage>& slices_out) {
    ISMRMRD::Image<T> img;
    ds.readImage("image_0", index, img);
    append_image_slices(img, slices_out);
    return true;
}

static bool read_dataset_image(ISMRMRD::Dataset& ds,
                               uint32_t index,
                               uint16_t data_type,
                               std::vector<SliceImage>& slices_out) {
    switch (data_type) {
    case ISMRMRD::ISMRMRD_USHORT: {
        return read_dataset_image_typed<uint16_t>(ds, index, slices_out);
    }
    case ISMRMRD::ISMRMRD_SHORT: {
        return read_dataset_image_typed<int16_t>(ds, index, slices_out);
    }
    case ISMRMRD::ISMRMRD_UINT: {
        return read_dataset_image_typed<uint32_t>(ds, index, slices_out);
    }
    case ISMRMRD::ISMRMRD_INT: {
        return read_dataset_image_typed<int32_t>(ds, index, slices_out);
    }
    case ISMRMRD::ISMRMRD_FLOAT: {
        return read_dataset_image_typed<float>(ds, index, slices_out);
    }
    case ISMRMRD::ISMRMRD_DOUBLE: {
        return read_dataset_image_typed<double>(ds, index, slices_out);
    }
    case ISMRMRD::ISMRMRD_CXFLOAT: {
        return read_dataset_image_typed<complex_float_t>(ds, index, slices_out);
    }
    case ISMRMRD::ISMRMRD_CXDOUBLE: {
        return read_dataset_image_typed<complex_double_t>(ds, index, slices_out);
    }
    default:
        return false;
    }
}

static bool read_image_data_type(const std::string& path,
                                 uint32_t index,
                                 uint16_t& data_type) {
    ScopedH5Handle file(H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose);
    if (!file) return false;

    ScopedH5Handle dataset(H5Dopen2(file.id, "/dataset/image_0/header", H5P_DEFAULT), H5Dclose);
    if (!dataset) return false;

    ScopedH5Handle file_type(H5Dget_type(dataset.id), H5Tclose);
    ScopedH5Handle native_type(H5Tget_native_type(file_type.id, H5T_DIR_ASCEND), H5Tclose);
    ScopedH5Handle file_space(H5Dget_space(dataset.id), H5Sclose);
    if (!file_type || !native_type || !file_space) return false;

    hsize_t start[1] = {index};
    hsize_t count[1] = {1};
    if (H5Sselect_hyperslab(file_space.id, H5S_SELECT_SET, start, nullptr, count, nullptr) < 0) {
        return false;
    }

    ScopedH5Handle mem_space(H5Screate_simple(1, count, nullptr), H5Sclose);
    if (!mem_space) return false;

    ISMRMRD::ImageHeader header{};
    if (H5Dread(dataset.id, native_type.id, mem_space.id, file_space.id, H5P_DEFAULT, &header) < 0) {
        return false;
    }

    data_type = header.data_type;
    return true;
}

static bool read_latest_h5(const std::string& path, std::vector<SliceImage>& slices_out) {
    slices_out.clear();
    try {
        ISMRMRD::Dataset ds(path.c_str(), "/dataset", false);
        uint32_t n = ds.getNumberOfImages("image_0");
        if (n == 0 || n > 256) return false;

        for (uint32_t i = 0; i < n; ++i) {
            uint16_t data_type = 0;
            if (!read_image_data_type(path, i, data_type)) {
                std::cerr << "viz failed to read image header for index " << i << "\n";
                return false;
            }
            if (!read_dataset_image(ds, i, data_type, slices_out)) {
                std::cerr << "viz unsupported data_type: " << data_type << "\n";
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "viz H5 read error: " << e.what() << "\n";
        return false;
    }

    std::sort(slices_out.begin(), slices_out.end(), [](const auto& a, const auto& b) {
        return a.slice_idx < b.slice_idx;
    });
    return !slices_out.empty();
}

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    std::string http_url = "http://localhost:8080";
    double interval_s = 0.033;  // default 30 Hz
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--http" || arg == "-http") && i + 1 < argc)
            http_url = argv[++i];
        else if ((arg == "--interval" || arg == "-interval") && i + 1 < argc)
            interval_s = std::stod(argv[++i]);
    }
    auto poll_gap = std::chrono::milliseconds(static_cast<long>(interval_s * 1000.0));

    std::cout << "=== viz_client ===\n";
    std::cout << "HTTP URL: " << http_url << "\n";
    std::cout << "Poll interval: " << interval_s << "s\n";
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
    auto next_poll = std::chrono::steady_clock::now();
    std::string latest_response;

    while (true) {
        bool showing_error = false;

        // 1. Poll GET /image/latest
        auto now = std::chrono::steady_clock::now();
        if (now >= next_poll) {
            auto http = http_get(curl, http_url + "/image/latest");
            if (http.ok) latest_response = std::move(http.body);
            else latest_response.clear();
            next_poll = now + poll_gap;
        }

        if (!latest_response.empty()) {
            try {
                auto j = nlohmann::json::parse(latest_response);
                std::string path = j.value("path", "");
                bool is_error = j.value("error", false);

                if (!path.empty() && !is_error) {
                    // Detect changes via file mtime (atomic rename changes mtime reliably)
                    std::error_code ec;
                    auto mtime = std::filesystem::last_write_time(path, ec);
                    if (!ec && (path != last_path || mtime != last_mtime)) {
                        std::vector<SliceImage> new_volume;
                        if (read_latest_h5(path, new_volume)) {
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
                } else if (is_error && !path.empty()) {
                    cv::Mat err_img = cv::imread(path, cv::IMREAD_COLOR);
                    if (!err_img.empty()) {
                        const int target_w = 640;
                        const int target_h = 360;
                        cv::resize(err_img, err_img, cv::Size(target_w, target_h), 0, 0, cv::INTER_NEAREST);
                        cv::rectangle(err_img, cv::Rect(0, 0, target_w, target_h), {0, 0, 120}, 8);
                        cv::putText(err_img, "RECON FAILED", {70, 185},
                                    cv::FONT_HERSHEY_SIMPLEX, 1.6, {255, 255, 255}, 4);
                        cv::putText(err_img, "reconstruction connection lost", {95, 235},
                                    cv::FONT_HERSHEY_SIMPLEX, 0.7, {255, 255, 255}, 2);
                        cv::imshow("viz_client", err_img);
                        showing_error = true;
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "viz error: " << e.what() << "\n";
            }
        }

        // 2. Render selected spatial slice
        if (!showing_error && !current_volume.empty() &&
            selected_slice < static_cast<int>(current_volume.size())) {
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
        } else if (!showing_error) {
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
