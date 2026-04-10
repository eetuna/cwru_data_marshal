/*
 * File: clients/kspace_streamer/kspace_streamer_main.cpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Full C ISMRMRD scanner mock — sends k-space to marshal via new API
 *
 * Protocol: POST /header (XML) + POST /config + POST /frame per acquisition + POST /close
 * Each /frame body = 340B AcquisitionHeader + trajectory bytes + sample bytes
 * Multi-slice, noise scans, ACQ_FIRST/LAST_IN_SLICE flags.
 * No X-MRD-* headers. No /v1/* paths.
 */

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include <stdexcept>
#include <cstdlib>

#include <ismrmrd/ismrmrd.h>

#include "phantom.hpp"

namespace http = boost::beast::http;

struct Options {
    std::string base_url{"http://localhost:8080"};
    std::string config_name{"simplefft"};
    std::size_t volumes{0};             // 0 = infinite
    double interval{0.5};               // seconds between volumes
    std::uint16_t samples{128};
    std::uint16_t channels{1};
    std::uint16_t lines{128};
    std::uint16_t slices{1};
    std::size_t log_stride{1};
};

Options parse_args(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 < argc) return argv[++i];
            throw std::runtime_error("missing value for " + arg);
        };
        if (arg == "--http")          opt.base_url = next();
        else if (arg == "--config")   opt.config_name = next();
        else if (arg == "--volumes")  opt.volumes = std::stoull(next());
        else if (arg == "--interval") opt.interval = std::stod(next());
        else if (arg == "--samples")  opt.samples = static_cast<uint16_t>(std::stoi(next()));
        else if (arg == "--channels") opt.channels = static_cast<uint16_t>(std::stoi(next()));
        else if (arg == "--lines")    opt.lines = static_cast<uint16_t>(std::stoi(next()));
        else if (arg == "--slices")   opt.slices = static_cast<uint16_t>(std::stoi(next()));
        else if (arg == "--log-stride") opt.log_stride = std::stoull(next());
    }
    return opt;
}

struct HttpTarget {
    std::string host;
    std::string port;
};

HttpTarget parse_base_url(const std::string& url) {
    auto rest = url.substr(7); // strip http://
    auto slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    auto colon = hostport.find(':');
    if (colon == std::string::npos) return {hostport, "80"};
    return {hostport.substr(0, colon), hostport.substr(colon + 1)};
}

// Generate minimal ISMRMRD XML header
std::string make_xml_header(uint16_t nx, uint16_t ny, uint16_t nz, uint16_t ncoils) {
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\"?>\n"
        << "<ismrmrdHeader xmlns=\"http://www.ismrmrd.org/ISMRMRD\">\n"
        << "  <encoding>\n"
        << "    <encodedSpace>\n"
        << "      <matrixSize><x>" << nx << "</x><y>" << ny << "</y><z>" << nz << "</z></matrixSize>\n"
        << "      <fieldOfView_mm><x>256</x><y>256</y><z>5</z></fieldOfView_mm>\n"
        << "    </encodedSpace>\n"
        << "    <reconSpace>\n"
        << "      <matrixSize><x>" << nx << "</x><y>" << ny << "</y><z>" << nz << "</z></matrixSize>\n"
        << "      <fieldOfView_mm><x>256</x><y>256</y><z>5</z></fieldOfView_mm>\n"
        << "    </reconSpace>\n"
        << "    <encodingLimits>\n"
        << "      <kspace_encoding_step_1><minimum>0</minimum><maximum>" << (ny - 1) << "</maximum><center>" << (ny / 2) << "</center></kspace_encoding_step_1>\n"
        << "      <slice><minimum>0</minimum><maximum>" << (nz - 1) << "</maximum><center>" << (nz / 2) << "</center></slice>\n"
        << "    </encodingLimits>\n"
        << "  </encoding>\n"
        << "</ismrmrdHeader>\n";
    return oss.str();
}

int main(int argc, char** argv) {
    try {
        Options opt = parse_args(argc, argv);
        auto target = parse_base_url(opt.base_url);

        std::cout << "kspace_streamer: Starting\n"
                  << "  marshal: " << opt.base_url << "\n"
                  << "  config: " << opt.config_name << "\n"
                  << "  samples: " << opt.samples
                  << " channels: " << opt.channels
                  << " lines: " << opt.lines
                  << " slices: " << opt.slices << "\n";

        boost::asio::io_context ioc;
        boost::asio::ip::tcp::resolver resolver{ioc};
        boost::beast::tcp_stream stream{ioc};
        boost::beast::flat_buffer read_buffer;

        auto do_connect = [&](const char* reason) {
            boost::system::error_code close_ec;
            if (stream.socket().is_open()) {
                stream.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, close_ec);
                stream.socket().close(close_ec);
            }
            for (int attempt = 0; attempt < 10; ++attempt) {
                auto endpoints = resolver.resolve(target.host, target.port);
                boost::system::error_code ec;
                stream.connect(endpoints, ec);
                if (!ec) {
                    stream.socket().set_option(boost::asio::ip::tcp::no_delay(true));
                    stream.expires_never();
                    read_buffer.consume(read_buffer.size());
                    if (reason) std::cout << "kspace_streamer: connected (" << reason << ")\n";
                    return;
                }
                std::cerr << "kspace_streamer: connect failed, retrying...\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            throw std::runtime_error("unable to connect to marshal");
        };

        auto post = [&](const std::string& path, const std::string& body) -> bool {
            http::request<http::string_body> req{http::verb::post, path, 11};
            req.set(http::field::host, target.host);
            req.set(http::field::content_type, "application/octet-stream");
            req.keep_alive(true);
            req.body() = body;
            req.prepare_payload();

            boost::system::error_code ec;
            http::write(stream, req, ec);
            if (ec) return false;

            http::response<http::string_body> res;
            http::read(stream, read_buffer, res, ec);
            if (ec) return false;
            read_buffer.consume(read_buffer.size());
            return true;
        };

        auto post_binary = [&](const std::string& path, const uint8_t* data, size_t len) -> bool {
            http::request<http::vector_body<uint8_t>> req{http::verb::post, path, 11};
            req.set(http::field::host, target.host);
            req.set(http::field::content_type, "application/octet-stream");
            req.keep_alive(true);
            req.body().assign(data, data + len);
            req.prepare_payload();

            boost::system::error_code ec;
            http::write(stream, req, ec);
            if (ec) return false;

            http::response<http::string_body> res;
            http::read(stream, read_buffer, res, ec);
            if (ec) return false;
            read_buffer.consume(read_buffer.size());
            return true;
        };

        do_connect("startup");

        const size_t nx = opt.samples;
        const size_t ny = opt.lines;
        const size_t ncoils = opt.channels;
        const size_t header_bytes = sizeof(ISMRMRD::AcquisitionHeader);
        const size_t line_data_bytes = nx * ncoils * sizeof(std::complex<float>);

        // Scratch buffers
        std::vector<double> phantom_img;
        std::vector<std::complex<float>> slice_kspace(nx * ny);
        std::vector<std::complex<float>> line_data(nx * ncoils);
        std::mt19937 rng{std::random_device{}()};
        std::normal_distribution<float> gauss{0.0f, 0.05f};

        // Send /header + /config
        std::string xml = make_xml_header(opt.samples, opt.lines, opt.slices, opt.channels);
        if (!post("/header", xml)) {
            do_connect("header failed");
            post("/header", xml);
        }
        post("/config", opt.config_name);

        std::cout << "kspace_streamer: header+config sent\n";

        size_t volume_index = 0;
        const size_t total = opt.volumes;
        auto next_deadline = std::chrono::steady_clock::now();

        while (total == 0 || volume_index < total) {
            double rotation = 0.05 * static_cast<double>(volume_index);
            double brightness = 0.75 + 0.25 * std::sin(0.1 * static_cast<double>(volume_index));

            size_t acq_count = 0;
            for (uint16_t slice = 0; slice < opt.slices; ++slice) {
                double slice_rot = rotation + 0.1 * slice;
                double slice_wt = std::cos(0.5 * M_PI *
                    (static_cast<double>(slice) - (opt.slices - 1) / 2.0) /
                    std::max(1.0, (opt.slices - 1) / 2.0));
                double slice_bright = brightness * std::max(0.25, slice_wt * slice_wt);

                kspace_sim::build_shepp_logan(nx, ny, slice_rot, slice_bright, phantom_img);
                kspace_sim::image_to_kspace(phantom_img, nx, ny, slice_kspace);

                for (uint16_t line = 0; line < ny; ++line) {
                    ISMRMRD::AcquisitionHeader ahdr;
                    std::memset(&ahdr, 0, sizeof(ahdr));
                    ahdr.version = 1;
                    ahdr.number_of_samples = opt.samples;
                    ahdr.active_channels = opt.channels;
                    ahdr.available_channels = opt.channels;
                    ahdr.trajectory_dimensions = 0;
                    ahdr.sample_time_us = 10.0f;
                    ahdr.scan_counter = static_cast<uint32_t>(
                        (volume_index * opt.slices + slice) * ny + line);
                    ahdr.idx.slice = slice;
                    ahdr.idx.kspace_encode_step_1 = line;
                    ahdr.idx.repetition = static_cast<uint16_t>(volume_index % 65535);

                    ahdr.flags = 0;
                    if (line == 0)
                        ahdr.flags |= (1ULL << (ISMRMRD::ISMRMRD_ACQ_FIRST_IN_SLICE - 1));
                    if (line == ny - 1)
                        ahdr.flags |= (1ULL << (ISMRMRD::ISMRMRD_ACQ_LAST_IN_SLICE - 1));

                    // Build per-channel k-space line with noise
                    for (size_t ch = 0; ch < ncoils; ++ch) {
                        for (size_t s = 0; s < nx; ++s) {
                            auto& src = slice_kspace[line * nx + s];
                            line_data[ch * nx + s] = std::complex<float>(
                                src.real() + gauss(rng), src.imag() + gauss(rng));
                        }
                    }

                    // Wire format: header + samples (no trajectory for Cartesian)
                    std::vector<uint8_t> frame(header_bytes + line_data_bytes);
                    std::memcpy(frame.data(), &ahdr, header_bytes);
                    std::memcpy(frame.data() + header_bytes, line_data.data(), line_data_bytes);

                    if (!post_binary("/frame", frame.data(), frame.size())) {
                        do_connect("frame write error");
                        // Resend header+config on reconnect
                        post("/header", xml);
                        post("/config", opt.config_name);
                    }

                    ++acq_count;
                }
            }

            if (volume_index % opt.log_stride == 0)
                std::cout << "volume " << volume_index << ": " << acq_count << " acquisitions sent\n";

            ++volume_index;

            if (opt.interval > 0.0) {
                next_deadline += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(opt.interval));
                auto now = std::chrono::steady_clock::now();
                if (next_deadline > now)
                    std::this_thread::sleep_until(next_deadline);
                else
                    next_deadline = now;
            }
        }

        // POST /close
        post("/close", "");
        std::cout << "kspace_streamer: done, " << volume_index << " volumes sent, /close posted\n";

    } catch (const std::exception& e) {
        std::cerr << "kspace_streamer error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
