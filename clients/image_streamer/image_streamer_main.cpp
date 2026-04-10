/*
 * File: clients/image_streamer/image_streamer_main.cpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Send synthetic ISMRMRD images via POST /header+/config+/frame+/close
 *
 * Wire format per image: 198B ImageHeader + 8B attr_len + attr + pixels
 * Same format as python-ismrmrd-server Connection.send_image.
 */

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include <stdexcept>

#include <ismrmrd/ismrmrd.h>

namespace http = boost::beast::http;

struct Options {
    std::string base_url{"http://localhost:8080"};
    std::string config_name{"simplefft"};
    std::size_t frames{0};
    double interval{0.5};
    std::uint16_t nx{32};
    std::uint16_t ny{32};
    std::uint16_t nz{1};
    std::size_t log_stride{10};
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
        else if (arg == "--frames")   opt.frames = std::stoull(next());
        else if (arg == "--interval") opt.interval = std::stod(next());
        else if (arg == "--nx")       opt.nx = static_cast<uint16_t>(std::stoi(next()));
        else if (arg == "--ny")       opt.ny = static_cast<uint16_t>(std::stoi(next()));
        else if (arg == "--nz")       opt.nz = static_cast<uint16_t>(std::stoi(next()));
        else if (arg == "--size") {
            auto v = static_cast<uint16_t>(std::stoi(next()));
            opt.nx = opt.ny = v;
        }
    }
    return opt;
}

struct HttpTarget {
    std::string host;
    std::string port;
};

HttpTarget parse_base_url(const std::string& url) {
    auto rest = url.substr(7);
    auto slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    auto colon = hostport.find(':');
    if (colon == std::string::npos) return {hostport, "80"};
    return {hostport.substr(0, colon), hostport.substr(colon + 1)};
}

std::string make_xml_header(uint16_t nx, uint16_t ny, uint16_t nz) {
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\"?>\n"
        << "<ismrmrdHeader xmlns=\"http://www.ismrmrd.org/ISMRMRD\">\n"
        << "  <encoding><encodedSpace><matrixSize>"
        << "<x>" << nx << "</x><y>" << ny << "</y><z>" << nz << "</z>"
        << "</matrixSize></encodedSpace></encoding>\n"
        << "</ismrmrdHeader>\n";
    return oss.str();
}

int main(int argc, char** argv) {
    try {
        Options opt = parse_args(argc, argv);
        auto target = parse_base_url(opt.base_url);

        std::cout << "image_streamer: Starting"
                  << " nx=" << opt.nx << " ny=" << opt.ny << " nz=" << opt.nz << "\n";

        boost::asio::io_context ioc;
        boost::asio::ip::tcp::resolver resolver{ioc};
        boost::beast::tcp_stream stream{ioc};
        boost::beast::flat_buffer read_buffer;

        auto do_connect = [&](const char* reason) {
            boost::system::error_code ec;
            if (stream.socket().is_open()) {
                stream.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
                stream.socket().close(ec);
            }
            for (int attempt = 0; attempt < 10; ++attempt) {
                auto endpoints = resolver.resolve(target.host, target.port);
                stream.connect(endpoints, ec);
                if (!ec) {
                    stream.socket().set_option(boost::asio::ip::tcp::no_delay(true));
                    stream.expires_never();
                    read_buffer.consume(read_buffer.size());
                    if (reason) std::cout << "image_streamer: connected (" << reason << ")\n";
                    return;
                }
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
            read_buffer.consume(read_buffer.size());
            return !ec;
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
            read_buffer.consume(read_buffer.size());
            return !ec;
        };

        do_connect("startup");

        // POST /header + /config
        post("/header", make_xml_header(opt.nx, opt.ny, opt.nz));
        post("/config", opt.config_name);

        const size_t pixel_count = static_cast<size_t>(opt.nx) * opt.ny * opt.nz;
        const size_t pixel_bytes = pixel_count * sizeof(float);
        std::string attr_str = "";
        uint64_t attr_len = attr_str.size();

        std::vector<float> pixels(pixel_count);
        auto next_deadline = std::chrono::steady_clock::now();

        for (size_t frame = 0; opt.frames == 0 || frame < opt.frames; ++frame) {
            // Generate synthetic image
            float t = static_cast<float>(frame) * 0.1f;
            for (uint16_t z = 0; z < opt.nz; ++z) {
                float zf = static_cast<float>(z) / std::max<float>(1.f, opt.nz - 1.f);
                for (uint16_t y = 0; y < opt.ny; ++y) {
                    for (uint16_t x = 0; x < opt.nx; ++x) {
                        float xf = static_cast<float>(x) / opt.nx;
                        float yf = static_cast<float>(y) / opt.ny;
                        pixels[z * opt.nx * opt.ny + y * opt.nx + x] =
                            0.5f + 0.3f * std::sin(xf * 6.28f + t)
                                 + 0.2f * std::cos(yf * 6.28f - t)
                                 + 0.1f * zf;
                    }
                }
            }

            // Build wire-format image: 198B header + 8B attr_len + attr + pixels
            ISMRMRD::ImageHeader ihdr;
            std::memset(&ihdr, 0, sizeof(ihdr));
            ihdr.version = 1;
            ihdr.data_type = ISMRMRD::ISMRMRD_FLOAT;
            ihdr.matrix_size[0] = opt.nx;
            ihdr.matrix_size[1] = opt.ny;
            ihdr.matrix_size[2] = opt.nz;
            ihdr.channels = 1;
            ihdr.image_series_index = 0;
            ihdr.image_index = static_cast<uint16_t>(frame % 65535);

            size_t total = sizeof(ihdr) + sizeof(uint64_t) + attr_len + pixel_bytes;
            std::vector<uint8_t> wire(total);
            size_t off = 0;
            std::memcpy(wire.data() + off, &ihdr, sizeof(ihdr)); off += sizeof(ihdr);
            std::memcpy(wire.data() + off, &attr_len, sizeof(attr_len)); off += sizeof(attr_len);
            if (attr_len > 0) {
                std::memcpy(wire.data() + off, attr_str.data(), attr_len);
                off += attr_len;
            }
            std::memcpy(wire.data() + off, pixels.data(), pixel_bytes);

            if (!post_binary("/frame", wire.data(), wire.size())) {
                do_connect("frame error");
                post("/header", make_xml_header(opt.nx, opt.ny, opt.nz));
                post("/config", opt.config_name);
            }

            if (frame % opt.log_stride == 0)
                std::cout << "image " << frame << " sent\n";

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

        post("/close", "");
        std::cout << "image_streamer: done\n";

    } catch (const std::exception& e) {
        std::cerr << "image_streamer error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
