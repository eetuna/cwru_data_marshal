/*
 * File: src/marshal_main.cpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Main server binary — new API (v2)
 *
 * Flags: --http host:port, --ws-port N, --recon-host HOST, --recon-port N,
 *        --dump-dir PATH, --dump
 */

#undef LOG_COMPONENT
#define LOG_COMPONENT "marshal_main"
#include "logging.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>

#include "marshal_http.hpp"
#include "marshal_state.hpp"
#include "marshal_ws.hpp"
#include "mrd_io.hpp"
#include "mrd_tcp_listener.hpp"
#include "recon_forwarder.hpp"

namespace fs = std::filesystem;

static uint32_t png_crc32(const uint8_t* data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t png_adler32(const std::vector<uint8_t>& data)
{
    uint32_t a = 1, b = 0;
    for (uint8_t v : data) {
        a = (a + v) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static void append_be32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

static void append_png_chunk(std::vector<uint8_t>& out, const char type[4],
                             const std::vector<uint8_t>& data)
{
    append_be32(out, static_cast<uint32_t>(data.size()));
    size_t crc_start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    uint32_t crc = png_crc32(out.data() + crc_start, out.size() - crc_start);
    append_be32(out, crc);
}

static std::vector<uint8_t> build_error_png()
{
    constexpr uint32_t width = 512;
    constexpr uint32_t height = 288;

    std::vector<uint8_t> raw;
    raw.reserve((width * 3 + 1) * height);
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0); // PNG filter type: none
        for (uint32_t x = 0; x < width; ++x) {
            const bool border = x < 10 || y < 10 || x >= width - 10 || y >= height - 10;
            const bool diag = (x * height > y * width ? x * height - y * width : y * width - x * height) < 2500;
            const bool anti = ((width - 1 - x) * height > y * width
                               ? (width - 1 - x) * height - y * width
                               : y * width - (width - 1 - x) * height) < 2500;
            if (border || diag || anti) {
                raw.insert(raw.end(), {255, 255, 255});
            } else {
                raw.insert(raw.end(), {180, 0, 0});
            }
        }
    }

    std::vector<uint8_t> zlib;
    zlib.push_back(0x78); // zlib header: deflate, no compression/fastest
    zlib.push_back(0x01);
    size_t off = 0;
    while (off < raw.size()) {
        const size_t chunk = std::min<size_t>(65535, raw.size() - off);
        const bool final = off + chunk == raw.size();
        zlib.push_back(final ? 0x01 : 0x00); // stored block
        uint16_t len = static_cast<uint16_t>(chunk);
        uint16_t nlen = static_cast<uint16_t>(~len);
        zlib.push_back(static_cast<uint8_t>(len & 0xFF));
        zlib.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        zlib.push_back(static_cast<uint8_t>(nlen & 0xFF));
        zlib.push_back(static_cast<uint8_t>((nlen >> 8) & 0xFF));
        zlib.insert(zlib.end(), raw.begin() + off, raw.begin() + off + chunk);
        off += chunk;
    }
    append_be32(zlib, png_adler32(raw));

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    append_be32(ihdr, width);
    append_be32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0}); // 8-bit RGB
    append_png_chunk(png, "IHDR", ihdr);
    append_png_chunk(png, "IDAT", zlib);
    append_png_chunk(png, "IEND", {});
    return png;
}

static void write_error_png(const fs::path& dump_dir)
{
    auto png = build_error_png();
    auto path = mrd::live_recon_dir(dump_dir) / "latest_error.png";
    mrd::write_standalone_file(path, png.data(), png.size());
}

static std::vector<uint8_t> build_recon_failure_image_body()
{
    constexpr uint16_t nx = 128;
    constexpr uint16_t ny = 128;

    ISMRMRD::ImageHeader hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    hdr.version = 1;
    hdr.data_type = ISMRMRD::ISMRMRD_FLOAT;
    hdr.matrix_size[0] = nx;
    hdr.matrix_size[1] = ny;
    hdr.matrix_size[2] = 1;
    hdr.channels = 1;
    hdr.field_of_view[0] = static_cast<float>(nx);
    hdr.field_of_view[1] = static_cast<float>(ny);
    hdr.field_of_view[2] = 1.0f;
    hdr.image_type = ISMRMRD::ISMRMRD_IMTYPE_MAGNITUDE;
    hdr.image_series_index = 9999;

    const std::string attr =
        "<ismrmrdMeta><meta><name>DataRole</name><value>ReconFailure</value></meta>"
        "<meta><name>ErrorMessage</name><value>RECON FAILED</value></meta></ismrmrdMeta>\0";
    const uint64_t attr_len = static_cast<uint64_t>(attr.size());

    std::vector<float> pixels(static_cast<size_t>(nx) * ny, 0.05f);
    for (uint16_t y = 0; y < ny; ++y) {
        for (uint16_t x = 0; x < nx; ++x) {
            const bool border = x < 4 || y < 4 || x >= nx - 4 || y >= ny - 4;
            const bool diag = (x > y ? x - y : y - x) < 3;
            const bool anti = (x + y > nx - 3) && (x + y < nx + 3);
            if (border || diag || anti) {
                pixels[static_cast<size_t>(y) * nx + x] = 1.0f;
            }
        }
    }

    const size_t pixel_bytes = pixels.size() * sizeof(float);
    std::vector<uint8_t> body(mrd::IMAGE_HEADER_BYTES + sizeof(uint64_t) +
                              attr_len + pixel_bytes);
    size_t off = 0;
    std::memcpy(body.data() + off, &hdr, mrd::IMAGE_HEADER_BYTES);
    off += mrd::IMAGE_HEADER_BYTES;
    std::memcpy(body.data() + off, &attr_len, sizeof(uint64_t));
    off += sizeof(uint64_t);
    std::memcpy(body.data() + off, attr.data(), attr_len);
    off += attr_len;
    std::memcpy(body.data() + off, pixels.data(), pixel_bytes);
    return body;
}

namespace {

bool parse_host_port(const std::string& input, std::string& host, uint16_t& port)
{
    auto p = input.find(':');
    if (p == std::string::npos || p == 0 || p + 1 >= input.size())
        return false;
    host = input.substr(0, p);
    try {
        int parsed = std::stoi(input.substr(p + 1));
        if (parsed <= 0 || parsed > 65535) return false;
        port = static_cast<uint16_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// HTTP session (sync, one per connection)
// ---------------------------------------------------------------------------

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
using tcp = net::ip::tcp;

static void http_session(tcp::socket sock, MarshalState& state)
{
    try {
        beast::flat_buffer buffer;
        for (;;) {
            http::request<http::string_body> req;
            http::read(sock, buffer, req);

            http::response<http::string_body> res;
            bool got_response = false;

            handle_http_request(std::move(req), state, [&](auto&& r) {
                res = std::move(r);
                got_response = true;
            });

            if (got_response) {
                http::write(sock, res);
                if (!res.keep_alive()) break;
            }
        }
    } catch (const beast::system_error& e) {
        if (e.code() != http::error::end_of_stream)
            LOG_WARN("HTTP session error: " << e.what());
    } catch (const std::exception& e) {
        LOG_WARN("HTTP session error: " << e.what());
    }
}

int main(int argc, char** argv)
{
    // Defaults
    std::string http_bind = "0.0.0.0:8080";
    std::string dump_dir  = "./data";
    bool dump_enabled = false;
    std::string recon_host;
    uint16_t recon_port = 9002;
    uint16_t ws_port = 0;
    uint16_t mrd_port = 0;
    std::size_t max_body_size = 128ULL * 1024ULL * 1024ULL;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--http" && i + 1 < argc)
            http_bind = argv[++i];
        else if (a == "--ws-port" && i + 1 < argc)
            ws_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (a == "--mrd-port" && i + 1 < argc)
            mrd_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (a == "--recon-host" && i + 1 < argc)
            recon_host = argv[++i];
        else if (a == "--recon-port" && i + 1 < argc)
            recon_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (a == "--dump-dir" && i + 1 < argc)
            dump_dir = argv[++i];
        else if (a == "--dump")
            dump_enabled = true;
        else if (a == "--max-body-size" && i + 1 < argc)
            max_body_size = std::stoull(argv[++i]);
    }

    std::string http_host;
    uint16_t http_port = 0;
    if (!parse_host_port(http_bind, http_host, http_port)) {
        LOG_ERROR("Invalid --http bind (expected host:port)");
        return 1;
    }

    // State
    MarshalState state;
    state.http_host = http_host;
    state.http_port = http_port;
    state.ws_port = ws_port;
    state.dump_dir = dump_dir;
    state.dump_enabled = dump_enabled;
    if (state.dump_enabled)
        state.dump_recorder = std::make_unique<mrd::DumpRecorder>(state.dump_dir);
    state.recon_url = recon_host.empty() ? "" : recon_host + ":" + std::to_string(recon_port);
    state.max_body_bytes = max_body_size;
    state.live_scanner_writer = std::make_unique<mrd::LatestImageWriter>();
    state.live_recon_writer = std::make_unique<mrd::LatestImageWriter>();

    // Ensure umbrella layout exists on disk at startup.
    mrd::live_scanner_dir(state.dump_dir);
    mrd::live_recon_dir(state.dump_dir);
    mrd::dump_scanner_dir(state.dump_dir);
    mrd::dump_recon_dir(state.dump_dir);

    // Recon forwarder via MRD TCP (optional)
    std::unique_ptr<mrd::ReconForwarder> forwarder;
    if (!recon_host.empty()) {
        auto on_failure = [&state]() {
            write_error_png(state.dump_dir);
            auto png_path = mrd::live_recon_dir(state.dump_dir) / "latest_error.png";
            {
                std::lock_guard<std::mutex> lk(state.latest_image_mtx);
                state.latest_image_path = png_path.string();
                state.latest_image_error = true;
            }

            if (state.recon_failure_reported.exchange(true) == false) {
                auto body = build_recon_failure_image_body();
                try {
                    state.mrd_push_message(mrd::MRD_MESSAGE_ISMRMRD_IMAGE,
                                           body.data(), body.size());
                } catch (...) {}
            }
        };

        // Recon return callback: archive IMAGE messages for non-scanner clients,
        // and push every MRD return message back to the scanner.
        auto on_message = [&state](uint16_t tag, const void* data, size_t len) {
            try { state.mrd_push_message(tag, data, len); } catch (...) {}

            if (tag == mrd::MRD_MESSAGE_ISMRMRD_IMAGE) {
                handle_recon_image(state, data, len);
            } else if (tag == mrd::MRD_MESSAGE_ISMRMRD_WAVEFORM) {
                handle_recon_waveform(state, data, len);
            } else if (tag == mrd::MRD_MESSAGE_TEXT && state.dump_enabled && state.dump_recorder) {
                std::string text;
                if (len >= sizeof(uint32_t)) {
                    uint32_t text_len = 0;
                    std::memcpy(&text_len, data, sizeof(text_len));
                    if (len >= sizeof(uint32_t) + text_len) {
                        text.assign(static_cast<const char*>(data) + sizeof(uint32_t), text_len);
                        auto nul = text.find('\0');
                        if (nul != std::string::npos) text.resize(nul);
                    }
                }
                state.dump_recorder->append_recon_text(text);
            }
        };

        forwarder = std::make_unique<mrd::ReconForwarder>(
            recon_host, recon_port, on_message, on_failure);
    }

    // Log config
    {
        std::ostringstream cfg;
        cfg << "marshal v2 listening http=" << http_bind
            << " dump-dir=" << dump_dir
            << " dump=" << (dump_enabled ? "on" : "off")
            << " max_body=" << max_body_size;
        if (!recon_host.empty()) cfg << " recon=" << recon_host << ":" << recon_port;
        if (ws_port > 0) cfg << " ws-port=" << ws_port;
        if (mrd_port > 0) cfg << " mrd-port=" << mrd_port;
        LOG_INFO(cfg.str());
    }

    // IO + acceptor
    net::io_context ioc{1};
    tcp::acceptor acceptor{ioc, {net::ip::make_address(http_host), http_port}};
    acceptor.set_option(net::socket_base::reuse_address(true));

    // MRD TCP listener for real scanner (--mrd-port)
    std::unique_ptr<mrd::MrdTcpListener> mrd_listener;
    if (mrd_port > 0) {
        mrd_listener = std::make_unique<mrd::MrdTcpListener>(
            ioc, mrd_port, state, forwarder.get());
        // Hook: recon MRD return messages are pushed to the scanner via MRD TCP.
        state.mrd_push_message = [&mrd_listener](uint16_t tag, const void* data, size_t len) {
            if (mrd_listener) mrd_listener->push_message_to_scanner(tag, data, len);
        };
    }

    // Optional WS server
    std::unique_ptr<WsServer> ws_server;
    if (ws_port > 0) {
        tcp::endpoint ws_ep{net::ip::make_address("0.0.0.0"), ws_port};
        ws_server = std::make_unique<WsServer>(ioc, ws_ep, state);
        LOG_INFO("WebSocket server on port " << ws_port);
    }

    // Accept loop (spawn thread per connection)
    std::function<void()> do_accept;
    do_accept = [&]() {
        acceptor.async_accept([&](beast::error_code ec, tcp::socket sock) {
            if (!ec) {
                std::thread([s = std::move(sock), &state]() mutable {
                    http_session(std::move(s), state);
                }).detach();
            }
            do_accept();
        });
    };
    do_accept();

    // Graceful shutdown
    net::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code&, int signum) {
        LOG_INFO("Received signal " << signum << ", shutting down...");
        acceptor.close();
        if (forwarder) forwarder->stop();
        state.close_scan();
        ioc.stop();
    });

    ioc.run();

    LOG_INFO("Server stopped");
    return 0;
}
