#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <algorithm>
#include <cmath>
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

namespace http = boost::beast::http;

struct Options {
    std::string base_url{"http://localhost:8080"};
    std::string stream{"demo_stream"};
    std::size_t frames{0}; // 0 = infinite
    double interval{0.5};
    std::uint16_t nx{32};
    std::uint16_t ny{32};
    std::uint16_t nz{4};
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
        if (arg == "--http") {
            opt.base_url = next();
        } else if (arg == "--stream") {
            opt.stream = next();
        } else if (arg == "--frames") {
            opt.frames = static_cast<std::size_t>(std::stoull(next()));
        } else if (arg == "--interval") {
            opt.interval = std::stod(next());
        } else if (arg == "--dt-ms") {
            opt.interval = std::stod(next()) / 1000.0;
        } else if (arg == "--size") {
            auto val = next();
            opt.nx = static_cast<std::uint16_t>(std::stoi(val));
            opt.ny = opt.nx;
        } else if (arg == "--nx") {
            opt.nx = static_cast<std::uint16_t>(std::stoi(next()));
        } else if (arg == "--ny") {
            opt.ny = static_cast<std::uint16_t>(std::stoi(next()));
        } else if (arg == "--width") {
            opt.nx = static_cast<std::uint16_t>(std::stoi(next()));
        } else if (arg == "--height") {
            opt.ny = static_cast<std::uint16_t>(std::stoi(next()));
        } else if (arg == "--slices") {
            opt.nz = static_cast<std::uint16_t>(std::stoi(next()));
        } else if (arg == "--nslices") {
            opt.nz = static_cast<std::uint16_t>(std::stoi(next()));
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
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
    auto slash = rest.find('/') ;
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

        boost::asio::io_context ioc;
        boost::asio::ip::tcp::resolver resolver{ioc};
        boost::beast::tcp_stream stream{ioc};
        boost::beast::flat_buffer read_buffer;
        boost::beast::flat_buffer write_buffer;
        auto next_deadline = std::chrono::steady_clock::now();

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
                    std::cerr << "image_streamer: resolve failed (" << resolve_ec.message() << "), retrying\n";
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
                    if (reason)
                        std::cout << "image_streamer: connected (" << reason << ")\n";
                    return;
                }

                std::cerr << "image_streamer: connect failed (" << connect_ec.message() << "), retrying\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }

            throw std::runtime_error("image_streamer: unable to connect to marshal at " + opt.base_url);
        };

        connect_stream("startup");

        ISMRMRD::Image<float> img(opt.nx, opt.ny, opt.nz, 1);
        ISMRMRD::ImageHeader &head = img.getHead();
        head.channels = 1;
        head.data_type = ISMRMRD::ISMRMRD_FLOAT;
        head.matrix_size[0] = opt.nx;
        head.matrix_size[1] = opt.ny;
        head.matrix_size[2] = opt.nz;
        head.image_series_index = 1;

        float *data = img.getDataPtr();
        const std::size_t n_vox = static_cast<std::size_t>(opt.nx) * opt.ny * opt.nz;
        const std::size_t header_bytes = sizeof(ISMRMRD::ImageHeader);
        const std::size_t payload_bytes = n_vox * sizeof(float);
        std::vector<uint8_t> body(header_bytes + payload_bytes);
        std::size_t frame_index = 0;
        const std::size_t total_frames = opt.frames;
        const std::size_t log_stride = 30;

        while (total_frames == 0 || frame_index < total_frames) {
            head.image_index = static_cast<uint16_t>((frame_index % 65535) + 1);
            head.slice = static_cast<uint16_t>((opt.nz > 0) ? (frame_index % opt.nz) : 0);

            const double t = static_cast<double>(frame_index);
            for (std::size_t z = 0; z < opt.nz; ++z) {
                const double z_term = z * 0.35;
                for (std::size_t y = 0; y < opt.ny; ++y) {
                    const double y_term = y * 0.07;
                    for (std::size_t x = 0; x < opt.nx; ++x) {
                        const std::size_t idx = z * opt.ny * opt.nx + y * opt.nx + x;
                        const double base = static_cast<double>(x + y) / std::max<std::size_t>(1, opt.nx + opt.ny);
                        const double wave = std::sin(t * 0.25 + z_term + x * 0.05 + y_term);
                        data[idx] = static_cast<float>(base + 0.25 * wave);
                    }
                }
            }

            std::memcpy(body.data(), &head, header_bytes);
            std::memcpy(body.data() + header_bytes, data, payload_bytes);

            bool delivered = false;
            std::string ack_body;
            http::status ack_status = http::status::ok;

            for (int attempt = 0; attempt < 3 && !delivered; ++attempt) {
                http::request<http::vector_body<uint8_t>> req{http::verb::post, "/v1/ismrmrd/frame", 11};
                req.set(http::field::host, http_target.host);
                req.set(http::field::content_type, "application/octet-stream");
                req.set("X-MRD-Stream", opt.stream);
                req.keep_alive(true);
                req.body() = body;
                req.prepare_payload();

                boost::system::error_code write_ec;
                http::write(stream, req, write_ec);
                if (write_ec) {
                    std::cerr << "image_streamer: write failed (" << write_ec.message() << "), reconnecting\n";
                    write_buffer.consume(write_buffer.size());
                    connect_stream("write error");
                    continue;
                }

                if (write_buffer.size() != 0) {
                    write_buffer.consume(write_buffer.size());
                }

                http::response<http::string_body> res;
                boost::system::error_code read_ec;
                http::read(stream, read_buffer, res, read_ec);
                if (read_ec) {
                    std::cerr << "image_streamer: read failed (" << read_ec.message() << "), reconnecting\n";
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
                continue; // try frame again after reconnect attempts
            }

            if (ack_status != http::status::ok) {
                std::cerr << "image_streamer: server responded with " << static_cast<unsigned>(ack_status)
                          << " body=" << ack_body << "\n";
            } else if (frame_index == 0 || frame_index % log_stride == 0) {
                std::cout << "frame " << frame_index << " -> " << ack_body << "\n";
            }

            ++frame_index;
            if (total_frames != 0 && frame_index >= total_frames) {
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
    } catch (const std::exception &e) {
        std::cerr << "image_streamer error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
