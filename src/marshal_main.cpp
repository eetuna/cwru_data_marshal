/*
 * File: src/marshal_main.cpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Main server binary — new API (v2)
 *
 * Flags: --http host:port, --ws-port N, --recon-host HOST, --recon-port N,
 *        --dump-dir PATH, --dump, --recon-close-timeout-ms N,
 *        --slice-agent-host HOST (enables the slice command channel),
 *        --slice-agent-port N, --slice-resend-ms N
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

#include "dump_recorder.hpp"
#include "marshal_http.hpp"
#include "marshal_state.hpp"
#include "marshal_ws.hpp"
#include "live_image_recorder.hpp"
#include "live_image_store.hpp"
#include "mrd_io.hpp"
#include "mrd_tcp_listener.hpp"
#include "recon_forwarder.hpp"
#include "session_registry.hpp"
#include "slice_agent_client.hpp"
#include "slice_math.hpp"

// Every header above re-defines LOG_COMPONENT for its own log lines; restore
// ours so main()'s messages are tagged correctly.
#undef LOG_COMPONENT
#define LOG_COMPONENT "marshal_main"

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

// LOW/NIT #19: checked CLI parse. Rejects trailing garbage and out-of-range
// values (std::stoi throws on invalid input — we catch and treat as false).
bool checked_parse_uint16(const std::string& s, uint16_t& out) {
    try {
        size_t pos = 0;
        long v = std::stol(s, &pos);
        if (pos != s.size()) return false;      // trailing non-numeric
        if (v < 0 || v > 65535) return false;
        out = static_cast<uint16_t>(v);
        return true;
    } catch (...) { return false; }
}

bool checked_parse_uint32(const std::string& s, uint32_t& out) {
    try {
        size_t pos = 0;
        unsigned long long v = std::stoull(s, &pos);
        if (pos != s.size()) return false;
        if (v > std::numeric_limits<uint32_t>::max()) return false;
        out = static_cast<uint32_t>(v);
        return true;
    } catch (...) { return false; }
}

bool checked_parse_size(const std::string& s, std::size_t& out) {
    try {
        size_t pos = 0;
        unsigned long long v = std::stoull(s, &pos);
        if (pos != s.size()) return false;
        out = static_cast<std::size_t>(v);
        return true;
    } catch (...) { return false; }
}

bool parse_host_port(const std::string& input, std::string& host, uint16_t& port)
{
    auto p = input.find(':');
    if (p == std::string::npos || p == 0 || p + 1 >= input.size())
        return false;
    host = input.substr(0, p);
    // LOW/NIT #19: reject trailing garbage in the port field.
    return checked_parse_uint16(input.substr(p + 1), port) && port != 0;
}

} // namespace

// ---------------------------------------------------------------------------
// HTTP session (sync, one per connection)
// ---------------------------------------------------------------------------

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
using tcp = net::ip::tcp;

static void http_session(std::shared_ptr<tcp::socket> sock, MarshalState& state)
{
    try {
        beast::flat_buffer buffer;
        for (;;) {
            // MEDIUM #11: use a parser with body_limit set BEFORE http::read
            // so oversized bodies are rejected as they stream, not buffered
            // in full and then checked in the handler.
            http::request_parser<http::string_body> parser;
            parser.body_limit(state.max_body_bytes);
            http::read(*sock, buffer, parser);
            auto req = parser.release();

            http::response<http::string_body> res;
            bool got_response = false;

            handle_http_request(std::move(req), state, [&](auto&& r) {
                res = std::move(r);
                got_response = true;
            });

            if (got_response) {
                http::write(*sock, res);
                if (!res.keep_alive()) break;
            }
        }
    } catch (const beast::system_error& e) {
        // MEDIUM #11: body_too_large is an expected rejection, not a bug.
        if (e.code() == http::error::body_limit) {
            LOG_WARN("HTTP body exceeded max_body_bytes; connection closed");
        } else if (e.code() != http::error::end_of_stream &&
                   e.code() != net::error::operation_aborted) {
            LOG_WARN("HTTP session error: " << e.what());
        }
    } catch (const std::exception& e) {
        LOG_WARN("HTTP session error: " << e.what());
    }
}

int main(int argc, char** argv)
{
    // Defaults
    std::string http_bind = "0.0.0.0:8080";
    std::string dump_dir  = "./data";
    std::string latest_dir;      // empty = snapshots live under dump_dir
    bool dump_enabled = false;
    std::string recon_host;
    uint16_t recon_port = 9002;
    uint16_t ws_port = 0;
    uint16_t mrd_port = 0;
    std::size_t max_body_size = 128ULL * 1024ULL * 1024ULL;
    uint32_t recon_close_timeout_ms = 30000;
    uint32_t recon_connect_timeout_ms = 5000;
    mrd::SliceAgentConfig slice_cfg;           // host empty = channel disabled
    MarshalState::SliceAgentSettings slice_settings;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        // LOW/NIT #19: checked parsing for every numeric flag.
        if (a == "--http" && i + 1 < argc)
            http_bind = argv[++i];
        else if (a == "--ws-port" && i + 1 < argc) {
            if (!checked_parse_uint16(argv[++i], ws_port)) {
                LOG_ERROR("--ws-port: invalid value '" << argv[i] << "'");
                return 1;
            }
        } else if (a == "--mrd-port" && i + 1 < argc) {
            if (!checked_parse_uint16(argv[++i], mrd_port)) {
                LOG_ERROR("--mrd-port: invalid value '" << argv[i] << "'");
                return 1;
            }
        } else if (a == "--recon-host" && i + 1 < argc) {
            recon_host = argv[++i];
            // A flag-like value means the intended value was dropped by the
            // shell (classic cause: RECON_HOST set to an empty string in the
            // compose environment) and this flag swallowed the NEXT flag.
            if (recon_host.rfind("--", 0) == 0) {
                LOG_ERROR("--recon-host: got '" << recon_host
                          << "', which looks like a flag, not a hostname. "
                             "If RECON_HOST was set to an empty string, unset "
                             "it instead to use the default.");
                return 1;
            }
        } else if (a == "--recon-port" && i + 1 < argc) {
            if (!checked_parse_uint16(argv[++i], recon_port)) {
                LOG_ERROR("--recon-port: invalid value '" << argv[i] << "'");
                return 1;
            }
        } else if (a == "--dump-dir" && i + 1 < argc)
            dump_dir = argv[++i];
        else if (a == "--latest-dir" && i + 1 < argc)
            latest_dir = argv[++i];
        else if (a == "--dump")
            dump_enabled = true;
        else if (a == "--max-body-size" && i + 1 < argc) {
            if (!checked_parse_size(argv[++i], max_body_size)) {
                LOG_ERROR("--max-body-size: invalid value '" << argv[i] << "'");
                return 1;
            }
        }
        else if (a == "--recon-close-timeout-ms" && i + 1 < argc) {
            if (!checked_parse_uint32(argv[++i], recon_close_timeout_ms)) {
                LOG_ERROR("--recon-close-timeout-ms: invalid value '" << argv[i] << "'");
                return 1;
            }
        }
        else if (a == "--recon-connect-timeout-ms" && i + 1 < argc) {
            if (!checked_parse_uint32(argv[++i], recon_connect_timeout_ms)) {
                LOG_ERROR("--recon-connect-timeout-ms: invalid value '" << argv[i] << "'");
                return 1;
            }
        }
        else if (a == "--slice-agent-host" && i + 1 < argc) {
            slice_cfg.host = argv[++i];
            // Same footgun as --recon-host: an empty SLICE_AGENT_HOST in the
            // compose environment would make this flag swallow the next one.
            if (slice_cfg.host.rfind("--", 0) == 0) {
                LOG_ERROR("--slice-agent-host: got '" << slice_cfg.host
                          << "', which looks like a flag, not a hostname. "
                             "If SLICE_AGENT_HOST was set to an empty string, "
                             "unset it instead (the channel is off by default).");
                return 1;
            }
        }
        else if (a == "--slice-agent-port" && i + 1 < argc) {
            if (!checked_parse_uint16(argv[++i], slice_cfg.port) || slice_cfg.port == 0) {
                LOG_ERROR("--slice-agent-port: invalid value '" << argv[i] << "'");
                return 1;
            }
        }
        else if (a == "--slice-resend-ms" && i + 1 < argc) {
            if (!checked_parse_uint32(argv[++i], slice_cfg.resend_window_ms)) {
                LOG_ERROR("--slice-resend-ms: invalid value '" << argv[i] << "'");
                return 1;
            }
        }
        else {
            // Unknown argument, or a known value-taking flag given as the
            // last argument with its value missing. Silently ignoring these
            // let misconfigurations (e.g. an empty RECON_HOST shifting every
            // later argument) start a marshal that dials bogus targets.
            LOG_ERROR("Unknown or incomplete argument '" << a << "'");
            return 1;
        }
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
    state.latest_dir = latest_dir;
    state.dump_enabled = dump_enabled;
    if (state.dump_enabled)
        state.dump_recorder = std::make_unique<mrd::DumpRecorder>(state.dump_dir);
    state.recon_url = recon_host.empty() ? "" : recon_host + ":" + std::to_string(recon_port);
    state.max_body_bytes = max_body_size;
    state.recon_close_timeout_ms = recon_close_timeout_ms;
    // Live infra (latest snapshot + per-lane history recorders) is mutually
    // exclusive with dump mode. In dump mode the operator wants a pure
    // archive; running the live snapshot/history pipelines alongside dump
    // wastes work and confuses throughput tests.
    if (!state.dump_enabled) {
        state.latest_writer = std::make_unique<mrd::LatestImageWriter>();
        state.scanner_live.recorder = std::make_unique<mrd::LiveImageRecorder>(
            mrd::live_scanner_dir(state.dump_dir));
        state.recon_live.recorder = std::make_unique<mrd::LiveImageRecorder>(
            mrd::live_recon_dir(state.dump_dir));
    }

    // Ensure file directories used by the active mode only.
    if (state.dump_enabled) {
        mrd::dump_scanner_dir(state.dump_dir);
        mrd::dump_recon_dir(state.dump_dir);
    } else {
        mrd::live_scanner_dir(state.dump_dir);
        mrd::live_recon_dir(state.dump_dir);
    }

    // Recon forwarder via MRD TCP (optional)
    std::unique_ptr<mrd::ReconForwarder> forwarder;
    if (!recon_host.empty()) {
        auto on_failure = [&state]() {
            LOG_INFO("Finalizing recon lane after recon socket ended");
            if (state.dump_enabled) {
                if (state.dump_recorder) {
                    state.dump_recorder->close_lane(mrd::DumpLane::Recon);
                }
            } else {
                mrd::flush_live_lane(state, mrd::LiveLane::Recon);
            }
            mrd::mark_lane_finalized_after_eof(state, mrd::LiveLane::Recon);

            // Live mode only: write the latest_error.png snapshot under
            // live/from_reconstruction so /image/latest can return an error
            // marker. In dump mode there is no live snapshot path and
            // /image/latest returns 404, so skip the live filesystem write.
            if (!state.dump_enabled) {
                const auto latest_root = mrd::latest_base_dir(state);
                write_error_png(latest_root);
                auto png_path = mrd::live_recon_dir(latest_root) / "latest_error.png";
                std::lock_guard<std::mutex> lk(state.latest_image_mtx);
                state.latest_image_path = png_path.string();
                state.latest_image_error = true;
                // The error transition is a publish too: bump so
                // generation-gated pollers (and the latest.h5 ETag) see a
                // change and re-read instead of sitting on the last image.
                state.latest_image_generation.fetch_add(1);
            }

            if (state.recon_failure_reported.exchange(true) == false) {
                auto body = build_recon_failure_image_body();
                // MEDIUM #18: log exceptions instead of silently swallowing.
                try {
                    state.mrd_push_message(mrd::MRD_MESSAGE_ISMRMRD_IMAGE,
                                           body.data(), body.size());
                } catch (const std::exception& e) {
                    LOG_WARN("mrd_push_message (recon failure) threw: " << e.what());
                } catch (...) {
                    LOG_WARN("mrd_push_message (recon failure) threw unknown exception");
                }
            }
        };

        // Recon return callback: archive IMAGE messages for non-scanner clients,
        // and push every MRD return message back to the scanner.
        auto on_message = [&state](uint16_t tag, const void* data, size_t len) {
            // MEDIUM #18: log exceptions instead of silently swallowing.
            try { state.mrd_push_message(tag, data, len); }
            catch (const std::exception& e) {
                LOG_WARN("mrd_push_message (tag " << tag << ") threw: " << e.what());
            } catch (...) {
                LOG_WARN("mrd_push_message (tag " << tag << ") threw unknown exception");
            }

            if (tag == mrd::MRD_MESSAGE_ISMRMRD_IMAGE) {
                handle_recon_image(state, data, len);
            } else if (tag == mrd::MRD_MESSAGE_ISMRMRD_WAVEFORM) {
                handle_recon_waveform(state, data, len);
            } else if (tag == mrd::MRD_MESSAGE_TEXT && state.dump_enabled && state.dump_recorder) {
                // Pass the full wire body verbatim ([uint32 len][text+NUL])
                // so dump's byte-exact guarantee holds for recon TEXT too.
                const auto* p = static_cast<const uint8_t*>(data);
                std::vector<uint8_t> body(p, p + len);
                // HIGH #10: enqueue returns a result; log dropped at
                // callsite rather than only via post-hoc accessor.
                auto r = state.dump_recorder->append_recon_text(std::move(body));
                if (r == mrd::DumpEnqueueResult::Dropped) {
                    LOG_WARN("DUMP drop at enqueue (recon_text)");
                }
            }
        };

        forwarder = std::make_unique<mrd::ReconForwarder>(
            recon_host, recon_port, on_message, on_failure,
            recon_connect_timeout_ms);
        // GET /status hook. Safe lifetime: HTTP sessions are joined before
        // the forwarder is destroyed at end of main().
        state.recon_connected = [&forwarder] {
            return forwarder && forwarder->is_connected();
        };
    }

    // Slice agent client (optional): UI slice commands -> scanner-side
    // slice_agent --listen. Lazy: no socket until the first command.
    std::unique_ptr<mrd::SliceAgentClient> slice_agent;
    slice_settings.enabled = !slice_cfg.host.empty();
    state.slice_agent_cfg = slice_settings;
    if (slice_settings.enabled) {
        slice_agent = std::make_unique<mrd::SliceAgentClient>(slice_cfg);
        slice_agent->start();
        // Hooks are safe after stop(): post() returns 0 and wait() false once stopping.
        state.slice_agent_post = [&slice_agent](const slice_math::WireCommand& c) -> uint64_t {
            return slice_agent ? slice_agent->post(c) : 0u;
        };
        state.slice_agent_wait = [&slice_agent](uint64_t gen) {
            return slice_agent && slice_agent->wait(gen);
        };
        state.slice_agent_connected = [&slice_agent] {
            return slice_agent && slice_agent->connected();
        };
    }

    // Log config
    {
        std::ostringstream cfg;
        cfg << "marshal v2 listening http=" << http_bind
            << " dump-dir=" << dump_dir
            << " dump=" << (dump_enabled ? "on" : "off")
            << " max_body=" << max_body_size;
        if (!latest_dir.empty()) cfg << " latest-dir=" << latest_dir;
        if (!recon_host.empty()) cfg << " recon=" << recon_host << ":" << recon_port
                                     << " recon_close_timeout_ms=" << recon_close_timeout_ms
                                     << " recon_connect_timeout_ms=" << recon_connect_timeout_ms;
        if (ws_port > 0) cfg << " ws-port=" << ws_port;
        if (mrd_port > 0) cfg << " mrd-port=" << mrd_port;
        if (slice_settings.enabled) {
            cfg << " slice-agent=" << slice_cfg.host << ":" << slice_cfg.port;
        } else {
            cfg << " slice-agent=off";
        }
        LOG_INFO(cfg.str());
    }

    // IO + acceptor
    net::io_context ioc{1};
    tcp::acceptor acceptor{ioc, {net::ip::make_address(http_host), http_port}};
    acceptor.set_option(net::socket_base::reuse_address(true));

    // Tracked HTTP sessions so shutdown can cancel sockets + join threads
    // before MarshalState is destroyed. See MRI_MARSHAL_BUGS_FINAL #1-3.
    mrd::SessionRegistry http_sessions;

    // MRD TCP listener for real scanner (--mrd-port)
    std::unique_ptr<mrd::MrdTcpListener> mrd_listener;
    if (mrd_port > 0) {
        mrd_listener = std::make_unique<mrd::MrdTcpListener>(
            ioc, mrd_port, state, forwarder.get());
        // Hook: recon MRD return messages are pushed to the scanner via MRD TCP.
        state.mrd_push_message = [&mrd_listener](uint16_t tag, const void* data, size_t len) {
            if (!mrd_listener) return false;
            return mrd_listener->push_message_to_scanner(tag, data, len);
        };
    }

    // Optional WS server
    std::unique_ptr<WsServer> ws_server;
    if (ws_port > 0) {
        tcp::endpoint ws_ep{net::ip::make_address("0.0.0.0"), ws_port};
        ws_server = std::make_unique<WsServer>(ioc, ws_ep, state);
        LOG_INFO("WebSocket server on port " << ws_port);
    }

    // Accept loop: each session is tracked so shutdown can cancel + join.
    std::function<void()> do_accept;
    do_accept = [&]() {
        acceptor.async_accept([&](beast::error_code ec, tcp::socket sock) {
            if (!ec) {
                if (http_sessions.shutting_down()) {
                    boost::system::error_code ignore;
                    sock.shutdown(tcp::socket::shutdown_both, ignore);
                    sock.close(ignore);
                } else {
                    auto sock_ptr = std::make_shared<tcp::socket>(std::move(sock));
                    // Pre-register with a placeholder id; capture by value into the
                    // thread so it can unregister itself on normal exit.
                    std::shared_ptr<uint64_t> session_id = std::make_shared<uint64_t>(0);
                    mrd::TrackedSession ts;
                    ts.cancel = [sock_ptr]() {
                        boost::system::error_code ignore;
                        sock_ptr->shutdown(tcp::socket::shutdown_both, ignore);
                        sock_ptr->close(ignore);
                    };
                    ts.thread = std::thread([sock_ptr, &state, session_id, &http_sessions]() mutable {
                        http_session(sock_ptr, state);
                        http_sessions.unregister_session(*session_id);
                    });
                    // On a shutdown race (returns 0), register_session cancels
                    // and joins the session itself.
                    *session_id = http_sessions.register_session(std::move(ts));
                }
            }
            if (!http_sessions.shutting_down()) do_accept();
        });
    };
    do_accept();

    // Graceful shutdown. Order matters:
    //   1. stop accepting new connections (HTTP + MRD)
    //   1b. stop the slice-agent client (sends 0xDEAD while the network is
    //       still up; later submit() calls return false harmlessly)
    //   2. cancel + join in-flight sessions so they stop touching MarshalState
    //   3. stop the recon forwarder (joins its reader thread)
    //   4. flush live lanes + close scan state
    //   5. stop the io_context
    net::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code&, int signum) {
        LOG_INFO("Received signal " << signum << ", shutting down...");
        acceptor.close();
        if (slice_agent) slice_agent->stop();
        if (mrd_listener) mrd_listener->stop();
        http_sessions.shutdown_and_join();
        if (forwarder) forwarder->stop();
        mrd::flush_all_live_lanes(state);
        state.close_scan();
        ioc.stop();
    });

    // Resilient run loop: an exception escaping any completion handler
    // would otherwise propagate out of run() and kill the marshal.
    // Log it and resume; run() returns normally only when out of work
    // (i.e. after the signal handler stopped the context).
    for (;;) {
        try {
            ioc.run();
            break;
        } catch (const std::exception& e) {
            LOG_ERROR("io_context handler threw: " << e.what() << " — resuming");
        } catch (...) {
            LOG_ERROR("io_context handler threw unknown exception — resuming");
        }
    }

    // In case ioc.run() returned without the signal handler firing (e.g.
    // acceptor error), still drain HTTP sessions before state destructs.
    if (slice_agent) slice_agent->stop();
    http_sessions.shutdown_and_join();
    if (mrd_listener) mrd_listener->stop();

    LOG_INFO("Server stopped");
    return 0;
}
