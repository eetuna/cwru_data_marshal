/*
 * File: src/marshal_main.cpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Main server binary — new API (v2)
 *
 * Flags: --http host:port, --ws-port N, --recon-url URL, --dump-dir PATH
 */

#undef LOG_COMPONENT
#define LOG_COMPONENT "marshal_main"
#include "logging.hpp"

#include <atomic>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>

#include <boost/asio.hpp>

#include "marshal_http.hpp"
#include "marshal_state.hpp"
#include "marshal_ws.hpp"
#include "mrd_io.hpp"
#include "mrd_tcp_listener.hpp"
#include "recon_forwarder.hpp"

namespace fs = std::filesystem;

// Embedded reconstruction-failed PNG (minimal 1x1 red pixel PNG, ~67 bytes)
// In production this would be a proper 200x200 image compiled in via xxd.
// For now, we generate a placeholder at runtime.
static void write_error_png(const fs::path& dump_dir)
{
    // Minimal valid PNG: 1x1 red pixel
    static const uint8_t png[] = {
        0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A, // PNG signature
        0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52, // IHDR chunk
        0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
        0x08,0x02,0x00,0x00,0x00,0x90,0x77,0x53,
        0xDE,
        0x00,0x00,0x00,0x0C,0x49,0x44,0x41,0x54, // IDAT chunk
        0x08,0xD7,0x63,0xF8,0xCF,0xC0,0x00,0x00,
        0x00,0x02,0x00,0x01,0xE2,0x21,0xBC,0x33,
        0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44, // IEND chunk
        0xAE,0x42,0x60,0x82
    };
    auto path = mrd::recon_dir(dump_dir) / "latest_error.png";
    mrd::write_standalone_file(path, png, sizeof(png));
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

static void http_session(tcp::socket sock, MarshalState& state,
                         mrd::ReconForwarder* forwarder)
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

            // If POST /frame and forwarder exists, also forward
            if (req.method() == http::verb::post && req.target() == "/frame" && forwarder) {
                forwarder->post_frame(std::string(req.body()));
            }
            // POST /header forward
            if (req.method() == http::verb::post && req.target() == "/header" && forwarder) {
                forwarder->post_header(std::string(req.body()));
            }
            // POST /config forward
            if (req.method() == http::verb::post && req.target() == "/config" && forwarder) {
                forwarder->post_config(std::string(req.body()));
            }
            // POST /close forward
            if (req.method() == http::verb::post && req.target() == "/close" && forwarder) {
                forwarder->post_close();
            }

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
    std::string recon_url;
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
        else if (a == "--recon-url" && i + 1 < argc)
            recon_url = argv[++i];
        else if (a == "--dump-dir" && i + 1 < argc)
            dump_dir = argv[++i];
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
    state.recon_url = recon_url;
    state.max_body_bytes = max_body_size;

    // Ensure dump directories
    mrd::scanner_dir(state.dump_dir);
    mrd::recon_dir(state.dump_dir);

    // Recon forwarder (optional)
    std::unique_ptr<mrd::ReconForwarder> forwarder;
    if (!recon_url.empty()) {
        forwarder = std::make_unique<mrd::ReconForwarder>(
            recon_url,
            [&state, &dump_dir]() {
                // On recon failure: write error PNG, update latest_image
                write_error_png(state.dump_dir);
                auto png_path = mrd::recon_dir(state.dump_dir) / "latest_error.png";
                std::lock_guard<std::mutex> lk(state.latest_image_mtx);
                state.latest_image_path = png_path.string();
                state.latest_image_error = true;
            }
        );
    }

    // Log config
    {
        std::ostringstream cfg;
        cfg << "marshal v2 listening http=" << http_bind
            << " dump-dir=" << dump_dir
            << " max_body=" << max_body_size;
        if (!recon_url.empty()) cfg << " recon-url=" << recon_url;
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
        // Hook: when recon POSTs /image, push it to scanner via MRD TCP
        state.mrd_push_image = [&mrd_listener](const void* data, size_t len) {
            if (mrd_listener) mrd_listener->push_image_to_scanner(data, len);
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
                std::thread([s = std::move(sock), &state, fwd = forwarder.get()]() mutable {
                    http_session(std::move(s), state, fwd);
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
