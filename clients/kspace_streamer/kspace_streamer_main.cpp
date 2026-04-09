/*
 * File: clients/kspace_streamer/kspace_streamer_main.cpp
 * Project: CWRU Data Marshal
 * Purpose: Stream synthetic raw k-space data (AcquisitionHeader) to test reconstruction flow
 *
 * This client generates raw k-space data with valid ISMRMRD AcquisitionHeader
 * and sends it to the MRI marshal. The marshal will:
 * 1. Detect data type as ACQUISITION
 * 2. Forward to external reconstruction service (if configured)
 * 3. Store reconstructed image to SWMR
 *
 * Usage:
 *   ./kspace_streamer --http http://localhost:8080 --stream test_scan
 *
 * Testing:
 *   # Terminal 1: Mock reconstruction service
 *   python3 tests/mock_recon_service.py
 *
 *   # Terminal 2: Marshal with reconstruction enabled
 *   ./build/marshal --data ./data --recon-endpoint http://localhost:9002
 *
 *   # Terminal 3: K-space streamer
 *   ./build/kspace_streamer --http http://localhost:8080 --stream raw_scan
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
    std::string stream{"kspace_stream"};
    std::size_t readouts{0};        // 0 = infinite
    double interval{0.1};           // seconds between readouts
    std::uint16_t samples{64};      // samples per readout (k-space line width)
    std::uint16_t channels{1};      // active coil channels
    std::uint16_t lines{64};        // phase encode lines (total readouts per frame)
    std::uint16_t slices{1};        // number of slices per volume
    std::size_t log_stride{10};
};

Options parse_args(int argc, char **argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> const char * {
            if (i + 1 < argc) {
                return argv[++i];
            }
            throw std::runtime_error("missing value for " + arg);
        };
        if (arg == "--http" || arg == "--marshal") {
            opt.base_url = next();
        } else if (arg == "--stream") {
            opt.stream = next();
        } else if (arg == "--readouts" || arg == "--frames") {
            opt.readouts = static_cast<std::size_t>(std::stoull(next()));
        } else if (arg == "--interval") {
            opt.interval = std::stod(next());
        } else if (arg == "--dt-ms") {
            opt.interval = std::stod(next()) / 1000.0;
        } else if (arg == "--samples") {
            opt.samples = static_cast<std::uint16_t>(std::stoi(next()));
        } else if (arg == "--channels") {
            opt.channels = static_cast<std::uint16_t>(std::stoi(next()));
        } else if (arg == "--lines") {
            opt.lines = static_cast<std::uint16_t>(std::stoi(next()));
        } else if (arg == "--slices") {
            opt.slices = static_cast<std::uint16_t>(std::stoi(next()));
        } else if (arg == "--log-stride") {
            opt.log_stride = static_cast<std::size_t>(std::stoull(next()));
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "K-Space Streamer - Send raw k-space data to MRI marshal\n\n"
                      << "Usage: kspace_streamer [OPTIONS]\n\n"
                      << "Options:\n"
                      << "  --http URL       Marshal HTTP endpoint (default: http://localhost:8080)\n"
                      << "  --stream NAME    Stream identifier (default: kspace_stream)\n"
                      << "  --readouts N     Number of readouts to send, 0=infinite (default: 0)\n"
                      << "  --interval SEC   Seconds between readouts (default: 0.1)\n"
                      << "  --dt-ms MS       Interval in milliseconds\n"
                      << "  --samples N      Samples per readout line (default: 256)\n"
                      << "  --channels N     Active coil channels (default: 8)\n"
                      << "  --lines N        Phase encode lines per frame (default: 128)\n"
                      << "  --slices N       Number of slices per volume (default: 1)\n"
                      << "  --log-stride N   Log every Nth readout (default: 10)\n"
                      << "\nExample:\n"
                      << "  kspace_streamer --http http://localhost:8080 --stream raw_scan --channels 4 --slices 5\n";
            std::exit(0);
        } else {
            std::cerr << "Unknown option: " << arg << " (use --help for usage)\n";
            std::exit(1);
        }
    }
    return opt;
}

struct HttpTarget {
    std::string host;
    std::string port;
};

HttpTarget parse_base_url(const std::string &url) {
    if (url.rfind("http://", 0) != 0) {
        throw std::runtime_error("Only http:// URLs are supported");
    }
    auto rest = url.substr(7);
    auto slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    auto colon = hostport.find(':');
    std::string host;
    std::string port;
    if (colon == std::string::npos) {
        host = hostport;
        port = "80";
    } else {
        host = hostport.substr(0, colon);
        port = hostport.substr(colon + 1);
    }
    return {host, port};
}

int main(int argc, char **argv) {
    try {
        Options opt = parse_args(argc, argv);
        auto http_target = parse_base_url(opt.base_url);

        std::cout << "kspace_streamer: Starting\n"
                  << "  marshal: " << opt.base_url << "\n"
                  << "  stream: " << opt.stream << "\n"
                  << "  samples: " << opt.samples << "\n"
                  << "  channels: " << opt.channels << "\n"
                  << "  lines/frame: " << opt.lines << "\n"
                  << "  slices/volume: " << opt.slices << "\n"
                  << "  interval: " << opt.interval << "s\n";

        boost::asio::io_context ioc;
        boost::asio::ip::tcp::resolver resolver{ioc};
        boost::beast::tcp_stream stream{ioc};
        boost::beast::flat_buffer read_buffer;
        auto next_deadline = std::chrono::steady_clock::now();
        std::uint64_t session_counter = 0;
        std::string session_token;

        auto refresh_session_token = [&]() {
            ++session_counter;
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
            session_token = opt.stream + "-" + std::to_string(session_counter) + "-" + std::to_string(now_ms);
        };

        auto connect_stream = [&](const char *reason) {
            boost::system::error_code close_ec;
            if (stream.socket().is_open()) {
                stream.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, close_ec);
                stream.socket().close(close_ec);
            }

            for (int attempt = 0; attempt < 8; ++attempt) {
                boost::system::error_code resolve_ec;
                auto endpoints = resolver.resolve(http_target.host, http_target.port, resolve_ec);
                if (resolve_ec) {
                    std::cerr << "kspace_streamer: resolve failed (" << resolve_ec.message() << "), retrying\n";
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    continue;
                }

                boost::system::error_code connect_ec;
                stream.connect(endpoints, connect_ec);
                if (!connect_ec) {
                    stream.socket().set_option(boost::asio::ip::tcp::no_delay(true));
                    stream.expires_never();
                    read_buffer.consume(read_buffer.size());
                    next_deadline = std::chrono::steady_clock::now();
                    refresh_session_token();
                    if (reason)
                        std::cout << "kspace_streamer: connected (" << reason << ")\n";
                    return;
                }

                std::cerr << "kspace_streamer: connect failed (" << connect_ec.message() << "), retrying\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }

            throw std::runtime_error("kspace_streamer: unable to connect to marshal at " + opt.base_url);
        };

        connect_stream("startup");

        // Required fields for valid detection (see mrd_type_detector.hpp)
        const std::size_t header_bytes = sizeof(ISMRMRD::AcquisitionHeader);
        const std::size_t nx = static_cast<std::size_t>(opt.samples);
        const std::size_t ny = static_cast<std::size_t>(opt.lines);
        const std::size_t ncoils = static_cast<std::size_t>(opt.channels);
        const std::size_t line_samples = nx * ncoils;              // per-readout complex samples
        const std::size_t line_data_bytes = line_samples * sizeof(std::complex<float>);

        // One acquisition per phase-encode line per slice - real 2D k-space
        // that python-ismrmrd-server simplefft can reconstruct into an image.
        const std::size_t acquisitions_per_volume =
            static_cast<std::size_t>(opt.slices) * ny;

        // Scratch buffers reused every volume/slice.
        std::vector<double> phantom_img;
        std::vector<std::complex<float>> slice_kspace(nx * ny);
        std::vector<std::complex<float>> line_data(line_samples);

        std::size_t volume_index = 0;
        const std::size_t total_volumes = opt.readouts > 0 ? opt.readouts : 0;
        const std::size_t log_stride = opt.log_stride;

        while (total_volumes == 0 || volume_index < total_volumes) {
            // Build multi-slice k-space volume: one acquisition per phase
            // encode line per slice, full 2D k-space from a Shepp-Logan
            // phantom that rotates + brightness-modulates over time.
            std::vector<uint8_t> volume_body;
            volume_body.reserve(acquisitions_per_volume *
                                (header_bytes + line_data_bytes));

            const double rotation = 0.05 * static_cast<double>(volume_index);
            const double brightness =
                0.75 + 0.25 * std::sin(0.1 * static_cast<double>(volume_index));

            for (std::uint16_t slice = 0; slice < opt.slices; ++slice) {
                // Per-slice rotation offset so slices look distinct.
                const double slice_rot = rotation + 0.1 * static_cast<double>(slice);
                const double slice_wt =
                    std::cos(0.5 * M_PI *
                             (static_cast<double>(slice) -
                              (opt.slices - 1) / 2.0) /
                             std::max(1.0, (opt.slices - 1) / 2.0));
                const double slice_bright =
                    brightness * (slice_wt * slice_wt);

                kspace_sim::build_shepp_logan(nx, ny, slice_rot, slice_bright,
                                              phantom_img);
                kspace_sim::image_to_kspace(phantom_img, nx, ny, slice_kspace);

                for (std::uint16_t line = 0; line < ny; ++line) {
                    ISMRMRD::AcquisitionHeader acq_header;
                    std::memset(&acq_header, 0, sizeof(acq_header));

                    acq_header.version = 1;
                    acq_header.number_of_samples = opt.samples;
                    acq_header.active_channels = opt.channels;
                    acq_header.available_channels = opt.channels;
                    acq_header.trajectory_dimensions = 0;   // Cartesian
                    acq_header.sample_time_us = 10.0f;

                    acq_header.scan_counter = static_cast<uint32_t>(
                        (volume_index * opt.slices + slice) * ny + line);
                    acq_header.idx.slice = slice;
                    acq_header.idx.kspace_encode_step_1 = line;
                    acq_header.idx.repetition =
                        static_cast<uint16_t>(volume_index % 65535);

                    acq_header.flags = 0;
                    if (line == 0) {
                        acq_header.flags |=
                            (1ULL << (ISMRMRD::ISMRMRD_ACQ_FIRST_IN_SLICE - 1));
                    }
                    if (line == ny - 1) {
                        acq_header.flags |=
                            (1ULL << (ISMRMRD::ISMRMRD_ACQ_LAST_IN_SLICE - 1));
                    }

                    // Replicate this k-space line across all coil channels
                    // (simple uniform sensitivity - simplefft just sums them).
                    for (std::size_t ch = 0; ch < ncoils; ++ch) {
                        std::memcpy(&line_data[ch * nx],
                                    &slice_kspace[line * nx],
                                    nx * sizeof(std::complex<float>));
                    }

                    const uint8_t* header_ptr =
                        reinterpret_cast<const uint8_t*>(&acq_header);
                    volume_body.insert(volume_body.end(), header_ptr,
                                       header_ptr + header_bytes);

                    const uint8_t* data_ptr =
                        reinterpret_cast<const uint8_t*>(line_data.data());
                    volume_body.insert(volume_body.end(), data_ptr,
                                       data_ptr + line_data_bytes);
                }
            }

            // Send entire volume (all slices)
            bool delivered = false;
            std::string ack_body;
            http::status ack_status = http::status::ok;

            for (int attempt = 0; attempt < 3 && !delivered; ++attempt) {
                http::request<http::vector_body<uint8_t>> req{http::verb::post, "/v1/mrd/frame", 11};
                req.set(http::field::host, http_target.host);
                req.set(http::field::content_type, "application/octet-stream");
                req.set("X-MRD-Stream", opt.stream);
                if (!session_token.empty())
                    req.set("X-MRD-Session", session_token);
                req.keep_alive(true);
                req.body() = std::move(volume_body);
                req.prepare_payload();

                boost::system::error_code write_ec;
                http::write(stream, req, write_ec);
                if (write_ec) {
                    std::cerr << "kspace_streamer: write failed (" << write_ec.message() << "), reconnecting\n";
                    connect_stream("write error");
                    // Rebuild volume_body since we moved it
                    continue;
                }

                http::response<http::string_body> res;
                boost::system::error_code read_ec;
                http::read(stream, read_buffer, res, read_ec);
                if (read_ec) {
                    std::cerr << "kspace_streamer: read failed (" << read_ec.message() << "), reconnecting\n";
                    connect_stream("read error");
                    continue;
                }

                ack_status = res.result();
                ack_body = res.body();
                read_buffer.consume(read_buffer.size());
                delivered = true;

                if (!res.keep_alive()) {
                    connect_stream("server closed");
                }
            }

            if (!delivered) {
                continue;  // try volume again after reconnect attempts
            }

            // Log response
            if (ack_status == http::status::created || ack_status == http::status::ok) {
                if (volume_index == 0 || volume_index % log_stride == 0) {
                    std::cout << "volume " << volume_index
                              << " (" << opt.slices << " slices)"
                              << " -> HTTP " << static_cast<unsigned>(ack_status)
                              << "\n";
                }
            } else if (ack_status == http::status::not_implemented) {
                std::cerr << "kspace_streamer: Marshal returned 501 Not Implemented\n"
                          << "  This means raw k-space was detected but no reconstruction service is configured.\n"
                          << "  Start marshal with: --recon-endpoint http://localhost:9002\n"
                          << "  Response: " << ack_body << "\n";
            } else {
                std::cerr << "kspace_streamer: server responded with HTTP "
                          << static_cast<unsigned>(ack_status)
                          << " body=" << ack_body << "\n";
            }

            ++volume_index;
            if (total_volumes != 0 && volume_index >= total_volumes) {
                break;
            }

            if (opt.interval > 0.0) {
                next_deadline += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(opt.interval));
                auto now = std::chrono::steady_clock::now();
                if (next_deadline > now) {
                    std::this_thread::sleep_until(next_deadline);
                } else {
                    next_deadline = now;
                }
            }
        }

        std::cout << "kspace_streamer: Finished sending " << volume_index << " volumes ("
                  << (volume_index * acquisitions_per_volume) << " total acquisitions)\n";

    } catch (const std::exception &e) {
        std::cerr << "kspace_streamer error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
