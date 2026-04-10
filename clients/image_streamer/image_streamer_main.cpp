/*
 * File: clients/image_streamer/image_streamer_main.cpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Send synthetic ISMRMRD images over MRD TCP
 *
 * Protocol: raw TCP to marshal --mrd-port using python-ismrmrd-server's
 * 2-byte message ID framing.
 *
 * Wire sequence:
 *   CONFIG_FILE(1)   — 2B tag + 1024B null-padded config name
 *   METADATA_XML(3)  — 2B tag + 4B length + XML bytes
 *   IMAGE(1022)      — 2B tag + 198B header + 8B attr_len + attr + pixels  (× N)
 *   CLOSE(4)         — 2B tag only
 *
 * Image wire format matches python-ismrmrd-server Connection.send_image.
 * No HTTP. No X-MRD-* headers. No /v1/* paths.
 */

#include <boost/asio.hpp>
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
#include "mrd_stream_tags.hpp"

namespace net = boost::asio;
using tcp = net::ip::tcp;

struct Options {
    std::string host{"localhost"};
    std::string port{"9100"};         // marshal --mrd-port
    std::string config_name{"simplefft"};
    std::size_t frames{0};            // 0 = infinite
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
        if (arg == "--host")           opt.host = next();
        else if (arg == "--port")      opt.port = next();
        else if (arg == "--config")    opt.config_name = next();
        else if (arg == "--frames")    opt.frames = std::stoull(next());
        else if (arg == "--interval")  opt.interval = std::stod(next());
        else if (arg == "--nx")        opt.nx = static_cast<uint16_t>(std::stoi(next()));
        else if (arg == "--ny")        opt.ny = static_cast<uint16_t>(std::stoi(next()));
        else if (arg == "--nz")        opt.nz = static_cast<uint16_t>(std::stoi(next()));
        else if (arg == "--size") {
            auto v = static_cast<uint16_t>(std::stoi(next()));
            opt.nx = opt.ny = v;
        }
        else if (arg == "--log-stride") opt.log_stride = std::stoull(next());
    }
    return opt;
}

// ── MRD TCP helpers ────────────────────────────────────────────

void write_all(tcp::socket& sock, const void* data, size_t len) {
    net::write(sock, net::buffer(data, len));
}

void send_config_file(tcp::socket& sock, const std::string& name) {
    uint8_t buf[2 + 1024] = {};
    uint16_t tag = mrd::MRD_MESSAGE_CONFIG_FILE;
    std::memcpy(buf, &tag, 2);
    std::memcpy(buf + 2, name.data(), std::min(name.size(), size_t(1024)));
    write_all(sock, buf, sizeof(buf));
}

void send_metadata_xml(tcp::socket& sock, const std::string& xml) {
    std::string with_nul = xml + '\0';
    uint16_t tag = mrd::MRD_MESSAGE_METADATA_XML_TEXT;
    uint32_t len = static_cast<uint32_t>(with_nul.size());
    write_all(sock, &tag, 2);
    write_all(sock, &len, 4);
    write_all(sock, with_nul.data(), len);
}

// Send IMAGE (tag 1022): 2B tag + 198B header + 8B attr_len + attr + pixels
void send_image(tcp::socket& sock, const ISMRMRD::ImageHeader& hdr,
                const std::string& attr, const void* pixels, size_t pixel_bytes) {
    uint16_t tag = mrd::MRD_MESSAGE_ISMRMRD_IMAGE;
    uint64_t attr_len = attr.size();
    write_all(sock, &tag, 2);
    write_all(sock, &hdr, sizeof(hdr));
    write_all(sock, &attr_len, sizeof(attr_len));
    if (attr_len > 0)
        write_all(sock, attr.data(), attr_len);
    write_all(sock, pixels, pixel_bytes);
}

void send_close(tcp::socket& sock) {
    uint16_t tag = mrd::MRD_MESSAGE_CLOSE;
    write_all(sock, &tag, 2);
}

std::string make_xml_header(uint16_t nx, uint16_t ny, uint16_t nz) {
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\"?>\n"
        << "<ismrmrdHeader xmlns=\"http://www.ismrmrd.org/ISMRMRD\">\n"
        << "  <experimentalConditions>\n"
        << "    <H1resonanceFrequency_Hz>123000000</H1resonanceFrequency_Hz>\n"
        << "  </experimentalConditions>\n"
        << "  <encoding>\n"
        << "    <encodedSpace><matrixSize>"
        << "<x>" << nx << "</x><y>" << ny << "</y><z>" << nz << "</z>"
        << "</matrixSize><fieldOfView_mm><x>256</x><y>256</y><z>5</z></fieldOfView_mm>"
        << "</encodedSpace>\n"
        << "    <reconSpace><matrixSize>"
        << "<x>" << nx << "</x><y>" << ny << "</y><z>" << nz << "</z>"
        << "</matrixSize><fieldOfView_mm><x>256</x><y>256</y><z>5</z></fieldOfView_mm>"
        << "</reconSpace>\n"
        << "    <trajectory>cartesian</trajectory>\n"
        << "  </encoding>\n"
        << "</ismrmrdHeader>\n";
    return oss.str();
}

// ── main ───────────────────────────────────────────────────────

int main(int argc, char** argv) {
    try {
        Options opt = parse_args(argc, argv);

        std::cout << "image_streamer: MRD TCP image mock\n"
                  << "  marshal: " << opt.host << ":" << opt.port << "\n"
                  << "  nx=" << opt.nx << " ny=" << opt.ny << " nz=" << opt.nz << "\n";

        net::io_context ioc;
        tcp::resolver resolver{ioc};
        tcp::socket sock{ioc};

        auto endpoints = resolver.resolve(opt.host, opt.port);
        net::connect(sock, endpoints);
        sock.set_option(tcp::no_delay(true));
        std::cout << "image_streamer: connected\n";

        // CONFIG_FILE + METADATA_XML
        send_config_file(sock, opt.config_name);
        send_metadata_xml(sock, make_xml_header(opt.nx, opt.ny, opt.nz));
        std::cout << "image_streamer: config+header sent\n";

        const size_t pixel_count = static_cast<size_t>(opt.nx) * opt.ny * opt.nz;
        const size_t pixel_bytes = pixel_count * sizeof(float);
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

            // Build ImageHeader
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

            // Send over MRD TCP
            send_image(sock, ihdr, "" /* empty attributes */, pixels.data(), pixel_bytes);

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

        send_close(sock);
        boost::system::error_code ec;
        sock.shutdown(tcp::socket::shutdown_both, ec);
        sock.close();
        std::cout << "image_streamer: done\n";

    } catch (const std::exception& e) {
        std::cerr << "image_streamer error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
