#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/core/flat_buffer.hpp>
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
        auto results = resolver.resolve(http_target.host, http_target.port);

        std::size_t frame_index = 0;
        const std::size_t total_frames = opt.frames;

        while (total_frames == 0 || frame_index < total_frames) {
            boost::asio::ip::tcp::socket socket{ioc};
            boost::asio::connect(socket, results.begin(), results.end());

            ISMRMRD::Image<float> img(opt.nx, opt.ny, opt.nz, 1);
            ISMRMRD::ImageHeader &head = img.getHead();
            head.channels = 1;
            head.data_type = ISMRMRD::ISMRMRD_FLOAT;
            head.matrix_size[0] = opt.nx;
            head.matrix_size[1] = opt.ny;
            head.matrix_size[2] = opt.nz;
            head.image_index = static_cast<uint16_t>((frame_index % 65535) + 1);
            head.image_series_index = 1;
            head.slice = static_cast<uint16_t>(frame_index % opt.nz);

            float *data = img.getDataPtr();
            const std::size_t n_vox = static_cast<std::size_t>(opt.nx) * opt.ny * opt.nz;
            const double t = static_cast<double>(frame_index);
            for (std::size_t z = 0; z < opt.nz; ++z) {
                for (std::size_t y = 0; y < opt.ny; ++y) {
                    for (std::size_t x = 0; x < opt.nx; ++x) {
                        const std::size_t idx = z * opt.ny * opt.nx + y * opt.nx + x;
                        const double base = (static_cast<double>(x + y) / (opt.nx + opt.ny));
                        const double wave = std::sin(t * 0.25 + z * 0.35 + x * 0.05 + y * 0.07);
                        data[idx] = static_cast<float>(base + 0.25 * wave);
                    }
                }
            }

            const std::size_t header_bytes = sizeof(ISMRMRD::ImageHeader);
            const std::size_t payload_bytes = n_vox * sizeof(float);

            std::vector<uint8_t> body;
            body.resize(header_bytes + payload_bytes);
            std::memcpy(body.data(), &head, header_bytes);
            std::memcpy(body.data() + header_bytes, data, payload_bytes);

            http::request<http::vector_body<uint8_t>> req{http::verb::post, "/v1/ismrmrd/frame", 11};
            req.set(http::field::host, http_target.host);
            req.set(http::field::content_type, "application/octet-stream");
            req.set("X-MRD-Stream", opt.stream);
            req.body() = body;
            req.prepare_payload();

            http::write(socket, req);

            boost::beast::flat_buffer buffer;
            http::response<http::string_body> res;
            http::read(socket, buffer, res);

            if (res.result() != http::status::ok) {
                std::cerr << "image_streamer: server responded with " << res.result_int()
                          << " body=" << res.body() << "\n";
            } else {
                std::cout << "frame " << frame_index << " -> " << res.body() << "\n";
            }

            boost::system::error_code ec;
            socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);

            ++frame_index;
            if (total_frames != 0 && frame_index >= total_frames) {
                break;
            }
            if (opt.interval > 0.0) {
                std::this_thread::sleep_for(std::chrono::duration<double>(opt.interval));
            }
        }
    } catch (const std::exception &e) {
        std::cerr << "image_streamer error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
