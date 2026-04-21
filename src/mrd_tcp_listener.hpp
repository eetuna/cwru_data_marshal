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
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>

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
    // returns EPIPE immediately; write_exact_fd reports failure, we
    // log, and the recon reader unwinds normally.
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
        // object alive; we then call write_exact_socket against the
        // socket object (not a cached native fd), so a concurrent
        // close from stop() produces a clean EBADF/EPIPE on the send
        // rather than a fd-reuse hazard.
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

        if (!write_exact_socket(*sock, &tag, sizeof(tag)) ||
            (len > 0 && !write_exact_socket(*sock, data, len))) {
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

    static bool write_exact_fd(int fd, const void* buf, size_t n) {
        const auto* p = static_cast<const uint8_t*>(buf);
        size_t done = 0;
        while (done < n) {
            ssize_t rc = ::send(fd, p + done, n - done, MSG_NOSIGNAL);
            if (rc < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (rc == 0) return false;
            done += static_cast<size_t>(rc);
        }
        return true;
    }

    // Blocking write on the tcp::socket object. Safe across concurrent
    // close() from stop(): if the socket closes mid-send, boost::asio
    // returns an error_code and we bail. Caller serializes via
    // scanner_send_mtx_ so multiple writers don't interleave bytes.
    static bool write_exact_socket(tcp::socket& sock, const void* buf, size_t n) {
        if (n == 0) return true;
        boost::system::error_code ec;
        boost::asio::write(sock, boost::asio::buffer(buf, n), ec);
        return !ec;
    }

    // writer_loop removed 2026-04-19 round-7: push_message_to_scanner
    // now calls write_exact_fd inline. See push_message_to_scanner's
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
                             << sock.remote_endpoint()
                             << " — another session is active");
                    boost::system::error_code ignore;
                    sock.shutdown(tcp::socket::shutdown_both, ignore);
                    sock.close(ignore);
                } else {
                    LOG_INFO("Scanner connected from " << accepted_socket->remote_endpoint());
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
                        *session_id = sessions_.register_session(std::move(ts));
                        if (*session_id == 0) {
                            ts.cancel();  // shutting down, just close
                        }
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
        std::vector<std::pair<uint16_t, std::vector<uint8_t>>> recon_preamble;
        bool normal_close_seen = false;

        auto send_or_buffer_recon = [&](uint16_t tag, const std::vector<uint8_t>& body) {
            if (!forwarder_) return;
            if (session_active_.load() && forwarder_->is_connected()) {
                forwarder_->post_frame(tag, body);
            } else {
                recon_preamble.emplace_back(tag, body);
            }
        };

        // One-shot recon session. begin_session is attempted at most
        // ONCE per scanner session: on the first frame that needs it.
        // If that attempt fails (or recon dies mid-scan), marshal does
        // NOT retry. Rationale:
        //   - Per-frame retries do a ~3s DNS/TCP timeout on each call,
        //     which wedges the scanner thread and hides scanner EOF.
        //   - Recon is a demo-style placeholder; once it's gone it's
        //     gone until the operator restarts the whole scan.
        //   - Scanner frames still archive locally (live/dump spool);
        //     only recon-side reconstructed images are lost for
        //     frames after recon died.
        bool recon_attempted = false;

        auto ensure_recon_session = [&]() -> bool {
            if (!forwarder_) return false;
            if (!session_active_.load() || !forwarder_->is_connected()) {
                if (recon_attempted) return false;   // one-shot, no retry
                recon_attempted = true;
                session_active_.store(forwarder_->begin_session());
                if (!session_active_.load()) return false;
                for (const auto& [tag, body] : recon_preamble) {
                    forwarder_->post_frame(tag, body);
                }
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
                    if (forwarder_ && session_active_.load()) {
                        forwarder_->post_close();
                        forwarder_->wait_for_close(std::chrono::milliseconds(2000));
                        forwarder_->end_session();
                    }
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
                    if (ensure_recon_session()) {
                        forwarder_->post_frame(MRD_MESSAGE_TEXT, body);
                    }
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
                    } else {
                        // Live mode: persist scanner waveforms (e.g. ECG) into
                        // per-lane live history alongside images.
                        append_live_waveform(state_, LiveLane::Scanner,
                                             body.data(), body.size());
                    }
                    if (ensure_recon_session()) {
                        forwarder_->post_frame(MRD_MESSAGE_ISMRMRD_WAVEFORM, body);
                    }
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
            LOG_INFO("Finalizing scanner lane after scanner socket ended");
            if (state_.dump_enabled) {
                if (state_.dump_recorder) {
                    state_.dump_recorder->close_lane(DumpLane::Scanner);
                }
            } else {
                flush_live_lane(state_, LiveLane::Scanner);
            }
            mark_lane_finalized_after_eof(state_, LiveLane::Scanner);
            if (!recon_was_active) {
                if (state_.dump_enabled) {
                    if (state_.dump_recorder) {
                        state_.dump_recorder->close_lane(DumpLane::Recon);
                    }
                } else {
                    flush_live_lane(state_, LiveLane::Recon);
                }
                mark_lane_finalized_after_eof(state_, LiveLane::Recon);
            }
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
