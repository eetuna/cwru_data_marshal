/*
 * File: src/mrd_tcp_listener.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Raw TCP MRD listener — drop-in for python-ismrmrd-server
 *
 * The scanner connects via raw TCP and speaks the MRD wire protocol
 * (see python-ismrmrd-server/connection.py + constants.py).
 *
 * Marshal reads messages, archives, forwards to recon via MRD TCP.
 * When recon sends images back, marshal pushes them to the scanner
 * over the same TCP socket using MRD wire format.
 */

#pragma once

#undef LOG_COMPONENT
#define LOG_COMPONENT "mrd_tcp"
#include "logging.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <boost/asio.hpp>

#include <ismrmrd/ismrmrd.h>
#include <ismrmrd/waveform.h>

#include "marshal_state.hpp"
#include "live_image_store.hpp"
#include "mrd_io.hpp"
#include "mrd_sink.hpp"
#include "mrd_stream_tags.hpp"
#include "mrd_type_detector.hpp"
#include "recon_forwarder.hpp"
#include "session_registry.hpp"
#include "wire_guards.hpp"

namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace mrd {

class MrdTcpListener {
public:
    MrdTcpListener(net::io_context& ioc, uint16_t port,
                   MarshalState& state, ReconForwarder* forwarder)
        : acceptor_(ioc, {tcp::v4(), port})
        , state_(state)
        , forwarder_(forwarder)
    {
        acceptor_.set_option(net::socket_base::reuse_address(true));
        LOG_INFO("MRD TCP listener on port " << port);
        do_accept();
    }

    ~MrdTcpListener() {
        stop();
    }

    // Called on graceful shutdown. Closes the acceptor + any active
    // scanner socket, then joins the session thread. Safe to call
    // multiple times.
    //
    // Interaction with push_message_to_scanner (inline send under
    // scanner_send_mtx_): stop() does NOT acquire scanner_send_mtx_.
    // A blocked push on a stuck scanner holds that mutex indefinitely
    // via the kernel ::send; taking the mutex here would deadlock
    // shutdown.
    //
    // Instead, stop() closes the scanner socket while the push may
    // still hold scanner_send_mtx_. On Linux, a blocked ::send on
    // a concurrently-closed fd returns EPIPE (or ECONNRESET); the
    // boost::asio::write wrapper surfaces that as error_code. The
    // push then logs a DEBUG message (it re-checks stopping_ to
    // pick DEBUG over WARN) and returns, releasing scanner_send_mtx_.
    //
    // Formally, concurrent close+write on the same boost::asio::socket
    // is outside the documented thread-safe operations -- but the
    // kernel-level behavior is well-defined and every real-world
    // implementation relies on it. We accept this pragmatism.
    void stop() {
        {
            std::lock_guard<std::mutex> lk(scanner_mtx_);
            if (stopping_) {
                // Already stopped; still make sure sessions are joined.
            } else {
                stopping_ = true;
            }
        }
        boost::system::error_code ignore;
        acceptor_.close(ignore);
        {
            std::lock_guard<std::mutex> lk(scanner_mtx_);
            if (scanner_socket_) {
                scanner_socket_->shutdown(tcp::socket::shutdown_both, ignore);
                scanner_socket_->close(ignore);
            }
        }
        sessions_.shutdown_and_join();
    }

    // Push a recon MRD return message back to the scanner synchronously.
    //
    // 2026-04-19 round-7: no in-process queue. This matches
    // python-ismrmrd-server and every standard MRD peer. The only
    // backpressure mechanism on this path is TCP flow control: if
    // the scanner socket's recv buffer fills, the kernel blocks
    // ::send here, which applies TCP backpressure back to recon
    // (the caller of this function is the recon reader thread, which
    // then blocks on the next recon frame). That is the correct
    // protocol-level mechanism the MRD streaming protocol relies on.
    //
    // Shutdown safety: stop() closes the scanner socket before
    // joining session threads. A blocked ::send on a closed fd
    // returns EPIPE immediately; the write reports failure, we log,
    // and the recon reader unwinds normally.
    //
    // The previous writer_thread_ (HIGH #4 2026-04) was meant to
    // avoid a shutdown deadlock caused by blocking ::send on a stuck
    // scanner. The correct fix for that is the socket-shutdown path
    // above, not an in-process queue. A queue re-introduces choices
    // between drop (contract violation) and unbounded growth (OOM)
    // that TCP flow control already solves.
    void push_message_to_scanner(uint16_t tag, const void* data, size_t len) {
        // scanner_send_mtx_ serializes the whole frame (tag + body) so
        // concurrent callers (e.g. recon reader thread + recon-failure
        // path on the session thread) can't interleave bytes on the
        // wire. It's held across the blocking send, which can take a
        // while on a slow scanner -- that's the intended TCP flow
        // control behavior (callers queue up naturally).
        std::lock_guard<std::mutex> send_lk(scanner_send_mtx_);

        // Grab the socket shared_ptr under scanner_mtx_, then release
        // that mutex so stop() can still close the socket concurrently
        // if it needs to. shared_ptr lifetime keeps the tcp::socket
        // object alive; we then write against the socket object (not a
        // cached native fd), so a concurrent close from stop() produces
        // a clean EBADF/EPIPE on the send rather than a fd-reuse hazard.
        std::shared_ptr<tcp::socket> sock;
        bool shutting = false;
        {
            std::lock_guard<std::mutex> lk(scanner_mtx_);
            shutting = stopping_;
            if (scanner_socket_ && scanner_socket_->is_open()) {
                sock = scanner_socket_;
            }
        }
        if (!sock) {
            LOG_DEBUG("Scanner writer: no scanner connected, drop tag=" << tag);
            return;
        }

        // Single gather write: tag + body leave in one syscall. Same
        // bytes on the wire as the previous two sequential writes; with
        // NODELAY set, this also avoids a tag-only segment for small
        // frames.
        const std::array<net::const_buffer, 2> frame{
            net::buffer(&tag, sizeof(tag)),
            net::buffer(data, len)};
        boost::system::error_code write_ec;
        net::write(*sock, frame, write_ec);
        const bool sent = !write_ec;
        if (sent) {
            // Track CLOSE delivery so the scanner CLOSE handler can
            // guarantee exactly one CLOSE per scan (relayed from recon
            // or emitted by the marshal itself, never both, never none).
            if (tag == MRD_MESSAGE_CLOSE) {
                close_sent_to_scanner_.store(true);
            }
        } else {
            // During shutdown the close is expected; log at DEBUG so
            // we don't paint the shutdown path with "failed to push"
            // warnings that look like a runtime error.
            bool shutting_now = false;
            {
                std::lock_guard<std::mutex> lk(scanner_mtx_);
                shutting_now = stopping_;
            }
            if (shutting || shutting_now) {
                LOG_DEBUG("Scanner writer: send failed during shutdown, tag="
                          << tag);
            } else {
                LOG_WARN("Scanner writer: failed to push MRD message tag=" << tag);
            }
            // Close the socket so the session unwinds, unless stop()
            // is already handling the close.
            boost::system::error_code ignore;
            if (!shutting_now) {
                sock->shutdown(tcp::socket::shutdown_both, ignore);
                sock->close(ignore);
            }
        }
    }

    bool has_scanner() const {
        std::lock_guard<std::mutex> lk(scanner_mtx_);
        return scanner_socket_ && scanner_socket_->is_open();
    }

private:
    tcp::acceptor acceptor_;
    MarshalState& state_;
    ReconForwarder* forwarder_;
    mutable std::mutex scanner_mtx_;
    // Serializes concurrent push_message_to_scanner calls so the
    // tag+body pair of a single MRD frame writes atomically to the
    // scanner socket. Held across the blocking send; other senders
    // queue on this mutex (TCP-style backpressure between internal
    // callers). NEVER acquired under scanner_mtx_; order is
    // scanner_send_mtx_ FIRST.
    std::mutex scanner_send_mtx_;
    std::shared_ptr<tcp::socket> scanner_socket_;
    std::atomic<bool> session_active_{false};
    // True once a CLOSE (relayed from recon or marshal-emitted) has been
    // written to the scanner for the CURRENT scan. Reset at each scan
    // boundary. Read after end_session() has joined the recon reader,
    // so no relay can race the check in the CLOSE handler.
    std::atomic<bool> close_sent_to_scanner_{false};
    bool stopping_{false};           // protected by scanner_mtx_
    SessionRegistry sessions_;

    // HIGH #10: act on DumpRecorder enqueue result at the moment of drop.
    // Log once per kind to avoid flooding. Returns true if accepted.
    template <typename Kind>
    bool check_dump_result(Kind kind_name, DumpEnqueueResult r) {
        if (r == DumpEnqueueResult::Accepted) return true;
        if (r == DumpEnqueueResult::Dropped) {
            if (!dump_dropped_logged_.exchange(true)) {
                LOG_WARN("DUMP drop at enqueue time (" << kind_name
                         << "); caller-visible backpressure signal");
            }
        }
        // DumpEnqueueResult::Stopped is expected during shutdown; no log.
        return false;
    }
    std::atomic<bool> dump_dropped_logged_{false};

    // remote_endpoint() THROWS if the peer already disconnected (race
    // between accept and the peer dropping). An exception here escapes
    // the accept handler, propagates out of io_context::run(), and
    // would kill the whole marshal. Use the error_code overload.
    static std::string peer_string(const tcp::socket& s) {
        boost::system::error_code ec;
        auto ep = s.remote_endpoint(ec);
        if (ec) return "<unknown>";
        std::ostringstream oss;
        oss << ep;
        return oss.str();
    }

    // Scanner-socket options, applied on accept.
    //
    // Keepalive probes detect a scanner that vanished without RST
    // (cable pull, power loss, console crash): the blocked read_exact
    // in handle_session fails after ~idle + intvl*cnt (~60 s) and the
    // session unwinds, freeing the single-scanner slot for a
    // reconnect. Without this, a half-open scanner socket blocked the
    // session forever and every new scanner connection was rejected
    // as "concurrent" until the marshal was restarted.
    //
    // NODELAY matches the recon-side socket (recon_forwarder.hpp) and
    // keeps small return frames (TEXT, CLOSE) from being Nagle-delayed.
    //
    // Keepalive only covers the IDLE case: while unACKed data sits in
    // the send queue (mid-scan pushback in flight when the scanner
    // vanishes), Linux suppresses keepalive probes and the ~15 min
    // retransmission timer governs instead — measured: a vanished
    // scanner with in-flight pushback stayed wedged past 9 min on
    // keepalive alone. TCP_USER_TIMEOUT bounds BOTH cases: the
    // connection is declared dead once transmitted data stays
    // unacknowledged for this long.
    static constexpr int      kScannerKeepIdleSec   = 30;
    static constexpr int      kScannerKeepIntvlSec  = 10;
    static constexpr int      kScannerKeepCnt       = 3;
    static constexpr unsigned kScannerUserTimeoutMs = 60000;

    static void configure_scanner_socket(tcp::socket& s) {
        boost::system::error_code ec;
        s.set_option(tcp::no_delay(true), ec);
        s.set_option(net::socket_base::keep_alive(true), ec);
        const int fd = s.native_handle();
        int v = kScannerKeepIdleSec;
        ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &v, sizeof(v));
        v = kScannerKeepIntvlSec;
        ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &v, sizeof(v));
        v = kScannerKeepCnt;
        ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &v, sizeof(v));
        unsigned ut = kScannerUserTimeoutMs;
        ::setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &ut, sizeof(ut));
    }

    // writer_loop removed 2026-04-19 round-7: push_message_to_scanner
    // now writes inline (single gather write of tag+body). See its
    // header comment for rationale.

    void do_accept() {
        acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket sock) {
            if (!ec) {
                std::shared_ptr<tcp::socket> accepted_socket;
                bool reject_concurrent = false;
                bool shutting = false;
                {
                    std::lock_guard<std::mutex> lk(scanner_mtx_);
                    if (stopping_) {
                        shutting = true;
                    } else if (scanner_socket_ && scanner_socket_->is_open()) {
                        // HIGH #8: another scanner is already connected. Do not
                        // overwrite scanner_socket_ — that would strand the
                        // first session's pushback path on a dead fd. Reject
                        // the newcomer cleanly instead.
                        reject_concurrent = true;
                    } else {
                        scanner_socket_ = std::make_shared<tcp::socket>(std::move(sock));
                        accepted_socket = scanner_socket_;
                    }
                }

                if (shutting) {
                    boost::system::error_code ignore;
                    sock.shutdown(tcp::socket::shutdown_both, ignore);
                    sock.close(ignore);
                } else if (reject_concurrent) {
                    LOG_WARN("Rejecting concurrent scanner connection from "
                             << peer_string(sock)
                             << " — another session is active");
                    boost::system::error_code ignore;
                    sock.shutdown(tcp::socket::shutdown_both, ignore);
                    sock.close(ignore);
                } else {
                    configure_scanner_socket(*accepted_socket);
                    LOG_INFO("Scanner connected from " << peer_string(*accepted_socket));
                    if (!sessions_.shutting_down()) {
                        auto socket_ptr = accepted_socket;
                        std::shared_ptr<uint64_t> session_id = std::make_shared<uint64_t>(0);
                        TrackedSession ts;
                        ts.cancel = [socket_ptr]() {
                            if (!socket_ptr) return;
                            boost::system::error_code ignore;
                            socket_ptr->shutdown(tcp::socket::shutdown_both, ignore);
                            socket_ptr->close(ignore);
                        };
                        ts.thread = std::thread([this, socket_ptr, session_id]() {
                            handle_session(socket_ptr);
                            sessions_.unregister_session(*session_id);
                        });
                        // On a shutdown race (returns 0), register_session
                        // cancels and joins the session itself; ts is
                        // moved-from either way and must not be touched.
                        *session_id = sessions_.register_session(std::move(ts));
                    }
                }
            }
            // Re-arm only if still accepting
            {
                std::lock_guard<std::mutex> lk(scanner_mtx_);
                if (stopping_) return;
            }
            if (acceptor_.is_open()) do_accept();
        });
    }

    static bool read_exact(tcp::socket& s, void* buf, size_t n) {
        boost::system::error_code ec;
        net::read(s, net::buffer(buf, n), ec);
        return !ec;
    }

    static bool read_exact(tcp::socket& s, std::vector<uint8_t>& out, size_t n) {
        out.resize(n);
        return read_exact(s, out.data(), n);
    }

    void handle_session(std::shared_ptr<tcp::socket> sock) {
        LOG_INFO("MRD session started");
        close_sent_to_scanner_.store(false);
        std::vector<std::pair<uint16_t, std::vector<uint8_t>>> recon_preamble;
        bool normal_close_seen = false;

        // Cap on WAVEFORM bytes held in the preamble. A k-space scan
        // flushes the preamble on its first ACQUISITION, so waveforms
        // buffered before that are negligible; the cap only matters for
        // long image-only scans with ECG, where recon is never engaged
        // and buffered waveforms would otherwise grow for the whole
        // scan. CONFIG/XML/TEXT are never dropped — they are tiny and
        // required for a correct recon session.
        static constexpr size_t kMaxPreambleWaveformBytes = 32ULL * 1024 * 1024;
        size_t preamble_waveform_bytes = 0;
        bool preamble_cap_logged = false;

        auto send_or_buffer_recon = [&](uint16_t tag, const std::vector<uint8_t>& body) {
            if (!forwarder_) return;
            if (session_active_.load() && forwarder_->is_connected()) {
                forwarder_->post_frame(tag, body);
                return;
            }
            if (tag == MRD_MESSAGE_ISMRMRD_WAVEFORM) {
                preamble_waveform_bytes += body.size();
                while (preamble_waveform_bytes > kMaxPreambleWaveformBytes) {
                    auto it = std::find_if(recon_preamble.begin(), recon_preamble.end(),
                        [](const auto& e) {
                            return e.first == MRD_MESSAGE_ISMRMRD_WAVEFORM;
                        });
                    if (it == recon_preamble.end()) break;
                    preamble_waveform_bytes -= it->second.size();
                    recon_preamble.erase(it);
                    if (!preamble_cap_logged) {
                        preamble_cap_logged = true;
                        LOG_WARN("Recon preamble waveform buffer exceeded "
                                 << kMaxPreambleWaveformBytes
                                 << " bytes; dropping oldest buffered waveforms "
                                 << "(recon not engaged this scan)");
                    }
                }
            }
            recon_preamble.emplace_back(tag, body);
        };

        // The preamble is strictly per-scan: it is flushed (and cleared)
        // when the first ACQUISITION opens the recon session, and cleared
        // at every scan boundary (CLOSE). Without the clears, scan B on a
        // persistent scanner connection replayed scan A's CONFIG/METADATA
        // into recon ahead of its own — recon then ran the stale config.
        auto clear_recon_preamble = [&]() {
            recon_preamble.clear();
            preamble_waveform_bytes = 0;
        };

        // Per contract (line 119): "Recon connection state must be
        // reset so a later scan can reconnect." Implementation:
        // begin_session() is attempted at most once per logical scan
        // (scanner MRD CLOSE resets the flag). If recon is down when
        // scanner starts a scan, marshal archives scanner frames
        // locally and skips forwarding until the scan ends. Operator
        // restarts recon between scans; the NEXT scan reconnects.
        bool recon_attempted = false;

        auto ensure_recon_session = [&]() -> bool {
            if (!forwarder_) return false;
            if (!session_active_.load() || !forwarder_->is_connected()) {
                if (recon_attempted) return false;   // one-shot per scan
                recon_attempted = true;
                session_active_.store(forwarder_->begin_session());
                if (!session_active_.load()) return false;
                for (const auto& [tag, body] : recon_preamble) {
                    forwarder_->post_frame(tag, body);
                }
                // Flushed onto the recon socket; drop the buffered copies
                // so they cannot be replayed into a later scan's session.
                clear_recon_preamble();
            }
            return session_active_.load() && forwarder_->is_connected();
        };

        try {
            while (sock->is_open()) {
                uint16_t msg_id = 0;
                if (!read_exact(*sock, &msg_id, sizeof(msg_id))) break;
                if (msg_id != MRD_MESSAGE_CLOSE) {
                    normal_close_seen = false;
                }

                switch (msg_id) {

                case MRD_MESSAGE_CONFIG_FILE: {
                    std::vector<uint8_t> body;
                    if (!read_exact(*sock, body, 1024)) goto done;
                    const char* buf = reinterpret_cast<const char*>(body.data());
                    std::string config(buf, strnlen(buf, 1024));
                    LOG_INFO("CONFIG_FILE: " << config);
                    {
                        std::lock_guard<std::mutex> lk(state_.scan_mtx);
                        state_.current_config = config;
                        state_.config_received.store(true);
                    }
                    if (state_.dump_enabled && state_.dump_recorder) {
                        // Pass the raw 1024-byte wire body verbatim;
                        // dump stores it as-is for byte-exact replay.
                        check_dump_result("config_file",
                            state_.dump_recorder->set_scanner_config_file(
                                std::vector<uint8_t>(body)));
                    }
                    send_or_buffer_recon(MRD_MESSAGE_CONFIG_FILE, body);
                    break;
                }

                case MRD_MESSAGE_CONFIG_TEXT: {
                    uint32_t len = 0;
                    if (!read_exact(*sock, &len, 4)) goto done;
                    size_t body_size = 0;
                    if (!validate_len_prefix_body(len, body_size)) {
                        LOG_WARN("CONFIG_TEXT rejected: len " << len << " exceeds cap");
                        goto done;
                    }
                    std::vector<uint8_t> body(body_size);
                    std::memcpy(body.data(), &len, 4);
                    if (len > 0 && !read_exact(*sock, body.data() + 4, len)) goto done;
                    const auto* payload = body.data() + 4;
                    std::string config(reinterpret_cast<const char*>(payload),
                                       reinterpret_cast<const char*>(payload) + len);
                    auto nul = config.find('\0');
                    if (nul != std::string::npos) config.resize(nul);
                    LOG_INFO("CONFIG_TEXT: " << config);
                    {
                        std::lock_guard<std::mutex> lk(state_.scan_mtx);
                        state_.current_config = config;
                        state_.config_received.store(true);
                    }
                    if (state_.dump_enabled && state_.dump_recorder) {
                        // Pass the full wire body ([uint32 len][text]);
                        // dump preserves NULs / unusual framing verbatim.
                        check_dump_result("config_text",
                            state_.dump_recorder->set_scanner_config_text(
                                std::vector<uint8_t>(body)));
                    }
                    send_or_buffer_recon(MRD_MESSAGE_CONFIG_TEXT, body);
                    break;
                }

                case MRD_MESSAGE_METADATA_XML_TEXT: {
                    uint32_t len = 0;
                    if (!read_exact(*sock, &len, 4)) goto done;
                    size_t body_size = 0;
                    if (!validate_len_prefix_body(len, body_size)) {
                        LOG_WARN("METADATA_XML_TEXT rejected: len " << len << " exceeds cap");
                        goto done;
                    }
                    std::vector<uint8_t> body(body_size);
                    std::memcpy(body.data(), &len, 4);
                    if (len > 0 && !read_exact(*sock, body.data() + 4, len)) goto done;
                    const auto* payload = body.data() + 4;
                    std::string xml(reinterpret_cast<const char*>(payload),
                                    reinterpret_cast<const char*>(payload) + len);
                    auto nul = xml.find('\0');
                    if (nul != std::string::npos) xml.resize(nul);
                    LOG_INFO("METADATA_XML: " << xml.size() << " bytes");
                    uint16_t nz = 1;
                    {
                        std::regex re_slice(R"(<slice>\s*<minimum>\d+</minimum>\s*<maximum>(\d+)</maximum>)");
                        std::smatch m;
                        if (std::regex_search(xml, m, re_slice)) {
                            nz = static_cast<uint16_t>(std::stoi(m[1].str()) + 1);
                        } else {
                            std::regex re_z(R"(<z>(\d+)</z>)");
                            if (std::regex_search(xml, m, re_z)) {
                                nz = static_cast<uint16_t>(std::stoi(m[1].str()));
                            }
                        }
                        if (nz == 0) nz = 1;
                    }
                    std::string scan_file;
                    {
                        std::lock_guard<std::mutex> lk(state_.scan_mtx);
                        if (state_.scanner_lane_finalized) {
                            state_.current_scan_filename.clear();
                            state_.scanner_lane_finalized = false;
                            state_.recon_lane_finalized = false;
                        }
                        if (state_.current_scan_filename.empty())
                            state_.current_scan_filename = scan_filename();
                        scan_file = state_.current_scan_filename;
                        state_.current_xml_header = xml;
                        state_.recon_expected_slices = nz;
                    }
                    if (state_.dump_enabled && state_.dump_recorder) {
                        // Pass the raw wire body for byte-exact
                        // METADATA_XML preservation in the spool.
                        // xml (NUL-stripped) is kept as an argument
                        // for dump's own sink header configuration;
                        // the spool record is the verbatim body.
                        check_dump_result("start_scan",
                            state_.dump_recorder->start_scan(
                                scan_file,
                                std::vector<uint8_t>(body),
                                xml));
                    }
                    // Live mode only: reset_live_outputs_for_new_scan touches
                    // live/ paths and creates per-lane directories. In dump
                    // mode there is no live snapshot/history to reset.
                    if (!state_.dump_enabled) {
                        reset_live_outputs_for_new_scan(state_);
                    }
                    state_.header_received.store(true);
                    send_or_buffer_recon(MRD_MESSAGE_METADATA_XML_TEXT, body);
                    break;
                }

                case MRD_MESSAGE_CLOSE: {
                    LOG_INFO("CLOSE");
                    // Who sends the scan-completion CLOSE back to the scanner
                    // depends on whether recon was engaged this scan. With a
                    // recon session active (k-space scan), recon's own CLOSE is
                    // relayed to the scanner by the recon reader. Without recon
                    // (image/metadata-only scan), marshal must emit the CLOSE
                    // itself, otherwise the scanner's receive side blocks waiting
                    // for a completion marker that never arrives.
                    const bool recon_engaged = forwarder_ && session_active_.load();
                    if (recon_engaged) {
                        forwarder_->post_close();
                        forwarder_->wait_for_close(
                            std::chrono::milliseconds(state_.recon_close_timeout_ms));
                        forwarder_->end_session();
                        // end_session() joined the recon reader, so any relayed
                        // recon CLOSE has already been pushed (and flagged). If
                        // none reached the scanner — recon died mid-scan, or its
                        // tail flush exceeded the timeout — emit the marshal's
                        // own CLOSE so the scanner's receive side never hangs
                        // waiting for a completion marker.
                        if (!close_sent_to_scanner_.load()) {
                            LOG_WARN("Recon session ended without CLOSE "
                                     "(failure or flush timeout of "
                                     << state_.recon_close_timeout_ms
                                     << " ms); emitting marshal CLOSE to scanner");
                            push_message_to_scanner(MRD_MESSAGE_CLOSE, nullptr, 0);
                        }
                    } else if (!close_sent_to_scanner_.load()) {
                        // 2-byte CLOSE marker only: ends THIS scan, not the TCP
                        // connection (which may carry a subsequent scan) and not
                        // the listening port.
                        push_message_to_scanner(MRD_MESSAGE_CLOSE, nullptr, 0);
                    }
                    // Scan boundary: the next scan on this connection gets a
                    // fresh CLOSE-delivery slate.
                    close_sent_to_scanner_.store(false);
                    // Allow the NEXT scan on this same scanner TCP
                    // connection to attempt a recon reconnect once.
                    // Recon may have been restarted by the operator
                    // between scans.
                    recon_attempted = false;
                    // Scan boundary: anything still buffered (image-only
                    // scan, or recon down all scan) belongs to THIS scan
                    // and must not leak into the next one's session.
                    clear_recon_preamble();
                    // HIGH #10: surface dump overflow to the operator via log
                    // before close_scan() tears the recorder down. Previously
                    // this was only visible as the dump_complete="false" HDF5
                    // attribute after the fact.
                    if (state_.dump_enabled && state_.dump_recorder
                        && state_.dump_recorder->had_overflow()) {
                        LOG_WARN("DUMP incomplete: dropped "
                                 << state_.dump_recorder->dropped_record_count()
                                 << " records / "
                                 << state_.dump_recorder->dropped_byte_count()
                                 << " bytes during this scan");
                    }
                    flush_all_live_lanes(state_);
                    state_.close_scan();
                    session_active_.store(false);
                    normal_close_seen = true;
                    break;
                }

                case MRD_MESSAGE_TEXT: {
                    uint32_t len = 0;
                    if (!read_exact(*sock, &len, 4)) goto done;
                    size_t body_size = 0;
                    if (!validate_len_prefix_body(len, body_size)) {
                        LOG_WARN("TEXT rejected: len " << len << " exceeds cap");
                        goto done;
                    }
                    std::vector<uint8_t> body(body_size);
                    std::memcpy(body.data(), &len, 4);
                    if (len > 0 && !read_exact(*sock, body.data() + 4, len)) goto done;
                    std::string text(reinterpret_cast<const char*>(body.data() + 4), len);
                    auto nul = text.find('\0');
                    if (nul != std::string::npos) text.resize(nul);
                    LOG_INFO("TEXT: " << text);
                    if (state_.dump_enabled && state_.dump_recorder) {
                        // Pass the verbatim wire body; dump preserves
                        // embedded NULs and unusual framing exactly.
                        check_dump_result("scanner_text",
                            state_.dump_recorder->append_scanner_text(
                                std::vector<uint8_t>(body)));
                    }
                    // Buffer for recon; do NOT open a recon session on TEXT.
                    // Only k-space (ACQUISITION) engages recon. If this scan is
                    // k-space, the buffered TEXT flushes ahead of the
                    // acquisitions; if it is an image/metadata-only scan, recon
                    // is never contacted.
                    send_or_buffer_recon(MRD_MESSAGE_TEXT, body);
                    break;
                }

                case MRD_MESSAGE_ISMRMRD_ACQUISITION: {
                    ISMRMRD::AcquisitionHeader ahdr;
                    if (!read_exact(*sock, &ahdr, ACQUISITION_HEADER_BYTES)) goto done;
                    size_t traj_bytes = size_t(ahdr.trajectory_dimensions)
                                      * ahdr.number_of_samples * sizeof(float);
                    std::vector<uint8_t> traj(traj_bytes);
                    if (traj_bytes > 0 && !read_exact(*sock, traj.data(), traj_bytes)) goto done;
                    size_t sample_bytes = size_t(ahdr.number_of_samples)
                                        * ahdr.active_channels * sizeof(complex_float_t);
                    std::vector<uint8_t> samples(sample_bytes);
                    if (!read_exact(*sock, samples.data(), sample_bytes)) goto done;

                    std::vector<uint8_t> body(ACQUISITION_HEADER_BYTES + traj_bytes + sample_bytes);
                    std::memcpy(body.data(), &ahdr, ACQUISITION_HEADER_BYTES);
                    if (traj_bytes > 0)
                        std::memcpy(body.data() + ACQUISITION_HEADER_BYTES,
                                    traj.data(), traj_bytes);
                    std::memcpy(body.data() + ACQUISITION_HEADER_BYTES + traj_bytes,
                                samples.data(), sample_bytes);

                    if (state_.dump_enabled && state_.dump_recorder) {
                        check_dump_result("scanner_acquisition",
                            state_.dump_recorder->append_scanner_acquisition(
                                ahdr, std::vector<uint8_t>(traj),
                                std::vector<uint8_t>(samples)));
                    }

                    // Forward raw bytes to recon with correct tag
                    if (ensure_recon_session()) {
                        forwarder_->post_frame(MRD_MESSAGE_ISMRMRD_ACQUISITION, body);
                    }
                    break;
                }

                case MRD_MESSAGE_ISMRMRD_IMAGE: {
                    std::vector<uint8_t> hdr_buf(IMAGE_HEADER_BYTES);
                    if (!read_exact(*sock, hdr_buf.data(), IMAGE_HEADER_BYTES)) goto done;
                    auto* ihdr = reinterpret_cast<const ISMRMRD::ImageHeader*>(hdr_buf.data());
                    uint64_t attr_len_raw = 0;
                    if (!read_exact(*sock, &attr_len_raw, 8)) goto done;

                    // HIGH #5/#6/#7: validate sizes before any allocation.
                    size_t attr_len = 0;
                    if (!validate_attr_len(attr_len_raw, attr_len)) {
                        LOG_WARN("IMAGE rejected: attr_len "
                                 << attr_len_raw << " exceeds cap");
                        goto done;
                    }
                    size_t pixel_bytes = 0;
                    if (!compute_pixel_bytes(ihdr->matrix_size[0],
                                             ihdr->matrix_size[1],
                                             ihdr->matrix_size[2],
                                             ihdr->channels,
                                             ISMRMRD::ismrmrd_sizeof_data_type(ihdr->data_type),
                                             pixel_bytes)) {
                        LOG_WARN("IMAGE rejected: invalid pixel-size product "
                                 << "dim=[" << ihdr->matrix_size[0] << ","
                                 << ihdr->matrix_size[1] << ","
                                 << ihdr->matrix_size[2] << "] ch="
                                 << ihdr->channels);
                        goto done;
                    }
                    size_t total = 0;
                    if (!compute_image_body_total(IMAGE_HEADER_BYTES,
                                                  attr_len, pixel_bytes, total)) {
                        LOG_WARN("IMAGE rejected: body total overflow or cap");
                        goto done;
                    }

                    std::vector<uint8_t> attr(attr_len);
                    if (attr_len > 0 && !read_exact(*sock, attr.data(), attr_len)) goto done;
                    std::vector<uint8_t> pixels(pixel_bytes);
                    if (!read_exact(*sock, pixels.data(), pixel_bytes)) goto done;

                    uint64_t attr_len_wire = static_cast<uint64_t>(attr_len);
                    std::vector<uint8_t> body(total);
                    size_t o = 0;
                    std::memcpy(body.data()+o, hdr_buf.data(), IMAGE_HEADER_BYTES); o += IMAGE_HEADER_BYTES;
                    std::memcpy(body.data()+o, &attr_len_wire, 8); o += 8;
                    if (attr_len > 0) { std::memcpy(body.data()+o, attr.data(), attr_len); o += attr_len; }
                    std::memcpy(body.data()+o, pixels.data(), pixel_bytes);

                    LOG_DEBUG("IMAGE from scanner: "
                              << ihdr->matrix_size[0] << "x" << ihdr->matrix_size[1]);
                    if (state_.dump_enabled) {
                        if (state_.dump_recorder) {
                            check_dump_result("scanner_image",
                                state_.dump_recorder->append_scanner_image(
                                    *ihdr, std::vector<uint8_t>(attr),
                                    std::vector<uint8_t>(pixels)));
                        }
                    } else {
                        // Scanner-origin IMAGE is already reconstructed image data.
                        // In live mode, save/expose it for file-reading clients;
                        // do not send it to the k-space reconstruction service.
                        append_live_image(state_, LiveLane::Scanner, body.data(), body.size());
                    }
                    break;
                }

                case MRD_MESSAGE_ISMRMRD_WAVEFORM: {
                    ISMRMRD::WaveformHeader whdr;
                    if (!read_exact(*sock, &whdr, WAVEFORM_HEADER_BYTES)) goto done;
                    // LOW/NIT #21: use sizeof(uint32_t) to match marshal_http.hpp:95
                    // and make any future wire-format change stand out.
                    size_t data_bytes = size_t(whdr.number_of_samples) * whdr.channels * sizeof(uint32_t);
                    std::vector<uint8_t> wf_data(data_bytes);
                    if (!read_exact(*sock, wf_data.data(), data_bytes)) goto done;

                    std::vector<uint8_t> body(WAVEFORM_HEADER_BYTES + data_bytes);
                    std::memcpy(body.data(), &whdr, WAVEFORM_HEADER_BYTES);
                    std::memcpy(body.data() + WAVEFORM_HEADER_BYTES, wf_data.data(), data_bytes);

                    if (state_.dump_enabled) {
                        if (state_.dump_recorder) {
                            check_dump_result("scanner_waveform",
                                state_.dump_recorder->append_scanner_waveform(
                                    whdr, std::vector<uint8_t>(wf_data)));
                        }
                    }
                    // Live mode: waveforms forward to recon but are not
                    // archived into the per-lane live H5. The archive was
                    // unusable anyway (HDF5 lock blocks mid-scan readers)
                    // and gated scanner TCP throughput via scan_mtx
                    // contention. See docs/fps-regression-2026-04-22.md.
                    // For a future ECG-with-image consumer (e.g. cardiac
                    // MPC), bundle waveforms into latest_image.h5 via an
                    // in-memory ring buffer; do not reinstate this spool.
                    // Buffer for recon; do NOT open a recon session on waveforms
                    // alone. Waveforms accompanying k-space flush once the first
                    // ACQUISITION opens the session (see send_or_buffer_recon).
                    send_or_buffer_recon(MRD_MESSAGE_ISMRMRD_WAVEFORM, body);
                    break;
                }

                default:
                    LOG_WARN("Unknown MRD message ID: " << msg_id);
                    goto done;
                }
            }
        } catch (const std::exception& e) {
            LOG_WARN("MRD session error: " << e.what());
        }

    done:
        if (!normal_close_seen) {
            const bool recon_was_active = session_active_.load();

            // Scanner died abnormally with a recon session engaged: end the
            // recon session NOW rather than leaving it dangling until the
            // next scan's begin_session() (the python server would sit
            // blocked in recv with its savedata file open). Send CLOSE so
            // recon finalizes its own side; the wait is short and fixed —
            // the scanner is gone, so recon's tail images have nowhere to
            // go and there is no one waiting on a longer flush.
            if (recon_was_active && forwarder_) {
                if (forwarder_->is_connected()) {
                    forwarder_->post_close();
                    forwarder_->wait_for_close(std::chrono::milliseconds(2000));
                }
                forwarder_->end_session();
            }

            LOG_INFO("Finalizing scanner lane after scanner socket ended");
            if (state_.dump_enabled) {
                if (state_.dump_recorder) {
                    state_.dump_recorder->close_lane(DumpLane::Scanner);
                }
            } else {
                flush_live_lane(state_, LiveLane::Scanner);
            }
            mark_lane_finalized_after_eof(state_, LiveLane::Scanner);
            // Recon lane finalize is unconditional: with the recon session
            // ended above, no more recon frames can arrive. If a recon
            // failure already finalized the lane via on_failure, doing it
            // again is benign (the close barrier is idempotent and the
            // converted spool is gone).
            if (state_.dump_enabled) {
                if (state_.dump_recorder) {
                    state_.dump_recorder->close_lane(DumpLane::Recon);
                }
            } else {
                flush_live_lane(state_, LiveLane::Recon);
            }
            mark_lane_finalized_after_eof(state_, LiveLane::Recon);
        }
        LOG_INFO("MRD session ended");
        session_active_.store(false);
        {
            std::lock_guard<std::mutex> lk(scanner_mtx_);
            scanner_socket_.reset();
        }
    }
};

} // namespace mrd
