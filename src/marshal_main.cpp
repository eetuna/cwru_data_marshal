/*
 * File: src/marshal_main.cpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Main server binary — new API (v2)
 *
 * Flags: --http host:port, --ws-port N, --mrd-port N,
 *        --recon-host HOST (required with --mrd-port), --recon-port N,
 *        --dump-dir PATH, --latest-dir PATH, --recon-connect-timeout-ms N,
 *        --dump (archive only, viewer off),
 *        --slice-agent-host HOST (enables the slice command channel),
 *        --slice-agent-port N, --slice-resend-ms N
 */

#undef LOG_COMPONENT
#define LOG_COMPONENT "marshal_main"
#include "logging.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
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
namespace net_check = boost::asio;

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

// One connect attempt to the recon address: "reachable" or the reason not.
std::string check_recon_reachable(const std::string& host, uint16_t port, uint32_t timeout_ms)
{
    net_check::io_context ioc;
    net_check::ip::tcp::socket sock(ioc);
    net_check::ip::tcp::resolver resolver(ioc);
    boost::system::error_code result = net_check::error::would_block;
    bool done = false;
    resolver.async_resolve(host, std::to_string(port),
        [&](const boost::system::error_code& ec, net_check::ip::tcp::resolver::results_type r) {
            if (ec) { result = ec; done = true; return; }
            net_check::async_connect(sock, r,
                [&](const boost::system::error_code& ec2, const net_check::ip::tcp::endpoint&) {
                    result = ec2;
                    done = true;
                });
        });
    ioc.run_for(std::chrono::milliseconds(timeout_ms));
    if (!done) return "no answer within " + std::to_string(timeout_ms) + " ms";
    boost::system::error_code ignore;
    sock.close(ignore);
    return result ? result.message() : "reachable";
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
    bool dump_mode = false;      // --dump: archive only, viewer off
    std::string recon_host;
    uint16_t recon_port = 9002;
    uint16_t ws_port = 0;
    uint16_t mrd_port = 0;
    std::size_t max_body_size = 128ULL * 1024ULL * 1024ULL;
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
            dump_mode = true;
        else if (a == "--max-body-size" && i + 1 < argc) {
            if (!checked_parse_size(argv[++i], max_body_size)) {
                LOG_ERROR("--max-body-size: invalid value '" << argv[i] << "'");
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

    // The marshal exists to sit between the scanner and recon: without a
    // recon target every scan would fail, so refuse to start instead of
    // failing quietly at the first scan.
    if (mrd_port > 0 && recon_host.empty()) {
        LOG_ERROR("--recon-host is required (where the reconstruction server listens)");
        return 1;
    }

    // State
    MarshalState state;
    if (const char* v = std::getenv("MARSHAL_VERSION"); v && *v) state.version = v;
    state.http_host = http_host;
    state.http_port = http_port;
    state.ws_port = ws_port;
    state.dump_dir = dump_dir;
    state.latest_dir = latest_dir;
    state.dump_mode = dump_mode;
    // Everything from the scanner and from recon is archived, always.
    state.dump_recorder = std::make_unique<mrd::DumpRecorder>(state.dump_dir);
    mrd::dump_scanner_dir(state.dump_dir);
    mrd::dump_recon_dir(state.dump_dir);
    state.snapshot = std::make_unique<mrd::LatestSnapshot>(
        mrd::live_recon_dir(mrd::latest_base_dir(state)));
    state.recon_url = recon_host.empty() ? "" : recon_host + ":" + std::to_string(recon_port);
    state.max_body_bytes = max_body_size;

    // Recon forwarder via MRD TCP
    std::unique_ptr<mrd::ReconForwarder> forwarder;
    if (!recon_host.empty()) {
        // A failed recon connection is reported to the scanner by the MRD
        // listener (RECON FAILED image + CLOSE) and to the viewer via status.
        auto on_failure = [&state]() {
            LOG_WARN("Recon connection failed");
            state.set_status("recon failed", "recon connection failed");
        };

        // Every recon message goes back to the scanner; images are also
        // archived and shown in the viewer, text and waveforms archived.
        auto on_message = [&state](uint16_t tag, const void* data, size_t len) {
            try { state.mrd_push_message(tag, data, len); }
            catch (const std::exception& e) {
                LOG_WARN("mrd_push_message (tag " << tag << ") threw: " << e.what());
            }
            const auto* p = static_cast<const uint8_t*>(data);
            if (tag == mrd::MRD_MESSAGE_ISMRMRD_IMAGE) {
                handle_recon_image(state, data, len);
            } else if (tag == mrd::MRD_MESSAGE_ISMRMRD_WAVEFORM) {
                state.dump_recorder->append_recon_waveform(std::vector<uint8_t>(p, p + len));
            } else if (tag == mrd::MRD_MESSAGE_TEXT) {
                state.dump_recorder->append_recon_text(std::vector<uint8_t>(p, p + len));
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
        cfg << "marshal " << state.version << " listening http=" << http_bind
            << " dump-dir=" << dump_dir
            << " dump=" << (dump_mode ? "on (viewer off)" : "off")
            << " max_body=" << max_body_size;
        if (!latest_dir.empty()) cfg << " latest-dir=" << latest_dir;
        if (!recon_host.empty()) cfg << " recon=" << recon_host << ":" << recon_port
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

    // Check the recon address once at startup so a wrong RECON_HOST shows
    // up in the log and /status before the first scan, not during it.
    if (!recon_host.empty()) {
        state.recon_check = check_recon_reachable(recon_host, recon_port,
                                                  recon_connect_timeout_ms);
        if (state.recon_check == "reachable")
            LOG_INFO("Recon " << state.recon_url << " is reachable");
        else
            LOG_WARN("Recon " << state.recon_url << " is NOT reachable: " << state.recon_check
                     << " — scans will fail until it is");
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
        state.close_scan();
        state.dump_recorder->wait_for_conversions();
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
