/*
 * ReconForwarder — forwards scanner MRD TCP messages to a reconstruction service.
 *
 * Design: one MRD TCP connection per scan, matching python-ismrmrd-server's
 * connection model. Scanner-side messages are written to recon in the same
 * order they are received. A reader thread concurrently drains recon return
 * messages so IMAGE/TEXT/WAVEFORM/CLOSE can be pushed back to the scanner.
 *
 * Lifecycle:
 *   1. Scanner sends CONFIG → marshal calls begin_session() → connects to recon
 *   2. Marshal forwards each message via send() → writes to the socket
 *   3. Reader thread reads recon responses → calls message callback
 *   4. Scanner sends CLOSE → marshal calls end_session() → sends CLOSE, waits for reader
 *   5. Next scan: new begin_session() → new connection
 */

#pragma once

#include <boost/asio.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <ismrmrd/ismrmrd.h>
#include <ismrmrd/waveform.h>

#include "mrd_stream_tags.hpp"
#include "wire_guards.hpp"
#include "logging.hpp"

namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace mrd {

class ReconForwarder {
public:
    // Every callback carries the IMMUTABLE epoch of the recon connection
    // that produced it (the value passed to begin_session). The reader
    // thread captures it at start and senders capture it under send_mtx_,
    // so a callback from an old connection can never observe a newer
    // epoch, whatever the listener has done since (audit #1 follow-up:
    // a mutable global stamp left a window around the next scan's first
    // ACQUISITION where an old callback read the new epoch).
    using MessageCallback = std::function<void(uint64_t session_epoch, uint16_t,
                                               const void*, size_t)>;
    using FailureCallback = std::function<void(uint64_t session_epoch)>;

    // Liveness bound on the CONNECTED recon socket (audit 2026-08-28 #5).
    // The scanner session thread writes acquisitions to recon
    // synchronously; a recon that accepts the connection but stops
    // reading (stuck GPU, paused process, wedged thread — socket still
    // open) fills its receive window and that write blocks forever,
    // which in turn stops the marshal consuming scanner input and blocks
    // shutdown. Keepalive covers the idle case; TCP_USER_TIMEOUT bounds
    // both unACKed data and the zero-window-probe state, so the kernel
    // aborts the connection (send/recv return an error → fail_recon)
    // once the peer has made no progress for this long. Same rationale
    // and values as configure_scanner_socket in mrd_tcp_listener.hpp.
    static constexpr int      kKeepIdleSec          = 30;
    static constexpr int      kKeepIntvlSec         = 10;
    static constexpr int      kKeepCnt              = 3;
    static constexpr uint32_t kDefaultUserTimeoutMs = 60000;

    ReconForwarder(const std::string& host, uint16_t port,
                   MessageCallback on_message, FailureCallback on_failure,
                   uint32_t connect_timeout_ms = 5000,
                   uint32_t user_timeout_ms = kDefaultUserTimeoutMs)
        : recon_host_(host), recon_port_(port),
          connect_timeout_ms_(connect_timeout_ms),
          user_timeout_ms_(user_timeout_ms),
          on_message_(std::move(on_message)), on_failure_(std::move(on_failure)) {}

    ~ReconForwarder() { end_session(); }

    // --- Session lifecycle (called from marshal's MRD TCP listener thread) ---

    // Connect to recon, start reader thread. Call ONCE per scan, before any send().
    //
    // Resolve + connect are BOUNDED by connect_timeout_ms_. A blackholed
    // recon host (firewall drop, dead VM) must not hold the scanner
    // session thread hostage for the kernel's SYN-retry period (~130 s
    // on default Linux); DNS resolution gets the same bound. The private
    // ioc_ is used only here — the reader/writer paths operate on the raw
    // native fd — so driving it with run_for on the calling thread is safe.
    bool begin_session(uint64_t session_epoch = 0) {
        end_session();  // clean up any previous session
        session_epoch_.store(session_epoch);
        drop_logged_.store(false);
        failure_reported_.store(false);
        close_received_.store(false);

        try {
            auto sock = std::make_unique<tcp::socket>(ioc_);
            tcp::resolver resolver(ioc_);
            boost::system::error_code result_ec = net::error::would_block;
            bool completed = false;

            resolver.async_resolve(recon_host_, std::to_string(recon_port_),
                [&](const boost::system::error_code& ec,
                    tcp::resolver::results_type results) {
                    if (ec) { result_ec = ec; completed = true; return; }
                    net::async_connect(*sock, results,
                        [&](const boost::system::error_code& ec2, const tcp::endpoint&) {
                            result_ec = ec2;
                            completed = true;
                        });
                });

            ioc_.restart();
            ioc_.run_for(std::chrono::milliseconds(connect_timeout_ms_));
            if (!completed) {
                // Deadline expired with the resolve/connect still pending.
                // Cancel and drain the aborted handlers (they capture the
                // locals above by reference, so they MUST run before this
                // scope exits), then record the timeout. The assignment
                // comes after the drain so a completion that raced the
                // deadline cannot resurrect a socket we just closed.
                resolver.cancel();
                boost::system::error_code ignore;
                sock->close(ignore);
                ioc_.run();
                result_ec = net::error::timed_out;
            }

            if (result_ec) {
                LOG_WARN("Failed to connect to recon at " << recon_host_ << ":"
                         << recon_port_ << " within " << connect_timeout_ms_
                         << " ms: " << result_ec.message());
                connected_.store(false);
                if (on_failure_) { try { on_failure_(session_epoch); } catch (...) {} }
                return false;
            }

            boost::system::error_code opt_ec;
            sock->set_option(tcp::no_delay(true), opt_ec);
            sock->set_option(net::socket_base::keep_alive(true), opt_ec);
            {
                const int fd = sock->native_handle();
                int v = kKeepIdleSec;
                ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &v, sizeof(v));
                v = kKeepIntvlSec;
                ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &v, sizeof(v));
                v = kKeepCnt;
                ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &v, sizeof(v));
                unsigned ut = user_timeout_ms_;
                ::setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &ut, sizeof(ut));
            }
            // async_connect leaves the native fd in non-blocking mode
            // (synchronous net::connect did not). The reader/writer
            // paths issue raw blocking recv/send on the native fd and
            // treat EAGAIN as a hard error, so restore blocking mode
            // now that no async operation remains on this socket.
            sock->native_non_blocking(false, opt_ec);
            if (opt_ec) {
                LOG_WARN("Failed to restore blocking mode on recon socket: "
                         << opt_ec.message());
            }
            {
                std::lock_guard<std::mutex> lk(socket_mtx_);
                socket_ = std::move(sock);
            }
            connected_.store(true);
            LOG_INFO("Connected to recon at " << recon_host_ << ":" << recon_port_);

            // Start reader thread for recon->scanner responses.
            reader_running_.store(true);
            reader_ = std::thread(&ReconForwarder::read_loop, this, session_epoch);
            return true;
        } catch (const std::exception& e) {
            LOG_WARN("Failed to connect to recon: " << e.what());
            connected_.store(false);
            if (on_failure_) { try { on_failure_(session_epoch); } catch (...) {} }
            return false;
        }
    }

    // Close connection, join reader thread. Call after CLOSE is sent.
    void end_session() {
        reader_running_.store(false);
        connected_.store(false);
        // Serialise against an in-flight epoch-checked send: after this
        // point no sender can have passed the require_epoch check for
        // the old connection and still be writing.
        { std::lock_guard<std::mutex> lk(send_mtx_); }

        // Shutdown wakes blocking send/recv. Close/reset only after the
        // reader thread has exited so the fd cannot be reused underneath it.
        shutdown_socket();

        if (reader_.joinable()) reader_.join();
        {
            std::lock_guard<std::mutex> lk(socket_mtx_);
            if (socket_ && socket_->is_open()) {
                boost::system::error_code ec;
                socket_->close(ec);
            }
            socket_.reset();
        }
    }

    // --- Message sending (called from marshal's MRD TCP listener thread) ---
    // These write synchronously to preserve python-server-like ordering.

    void send_config_file(const std::string& name) {
        uint8_t buf[2 + 1024] = {};
        uint16_t tag = MRD_MESSAGE_CONFIG_FILE;
        std::memcpy(buf, &tag, 2);
        std::memcpy(buf + 2, name.data(), std::min(name.size(), size_t(1024)));
        send_message(buf, sizeof(buf));
    }

    void send_config_text(const std::string& text) {
        std::string with_nul = text + '\0';
        uint32_t len = static_cast<uint32_t>(with_nul.size());
        std::vector<uint8_t> msg(2 + 4 + len);
        uint16_t tag = MRD_MESSAGE_CONFIG_TEXT;
        std::memcpy(msg.data(), &tag, 2);
        std::memcpy(msg.data() + 2, &len, 4);
        std::memcpy(msg.data() + 6, with_nul.data(), len);
        send_message(msg.data(), msg.size());
    }

    void send_header(const std::string& xml) {
        std::string with_nul = xml + '\0';
        uint32_t len = static_cast<uint32_t>(with_nul.size());
        std::vector<uint8_t> msg(2 + 4 + len);
        uint16_t tag = MRD_MESSAGE_METADATA_XML_TEXT;
        std::memcpy(msg.data(), &tag, 2);
        std::memcpy(msg.data() + 2, &len, 4);
        std::memcpy(msg.data() + 6, with_nul.data(), len);
        send_message(msg.data(), msg.size());
    }

    void send_frame(uint16_t tag, const void* data, size_t len) {
        std::vector<uint8_t> msg(2 + len);
        std::memcpy(msg.data(), &tag, 2);
        std::memcpy(msg.data() + 2, data, len);
        send_message(msg.data(), msg.size());
    }

    void send_close() {
        uint16_t tag = MRD_MESSAGE_CLOSE;
        send_message(&tag, 2);
    }

    // Legacy API names (used by mrd_tcp_listener)
    void post_header(const std::string& xml)    { send_header(xml); }
    void post_config(const std::string& config) { send_config_file(config); }
    void post_config_text(const std::string& t) { send_config_text(t); }
    void post_frame(uint16_t tag, const std::string& body) {
        send_frame(tag, body.data(), body.size());
    }
    void post_frame(uint16_t tag, const std::vector<uint8_t>& body) {
        send_frame(tag, body.data(), body.size());
    }
    void post_close() { send_close(); }

    // CLOSE only if `epoch` still owns this forwarder's connection. For
    // finalizers that run after the scanner slot was released: a newer
    // scan may already have begun its own session, and its recon must
    // not receive the old scan's CLOSE. Checked under send_mtx_, which
    // begin_session's end_session() also serialises against.
    void post_close_for(uint64_t epoch) {
        uint16_t tag = MRD_MESSAGE_CLOSE;
        send_message(&tag, 2, epoch);
    }

    uint64_t session_epoch() const { return session_epoch_.load(); }

    // Send a fully-assembled wire frame ([tag][body]) verbatim. Used by
    // the ACQUISITION hot path, whose bytes are already contiguous in the
    // listener's read buffer — avoids the per-message copy post_frame's
    // tag-prepend would cost.
    void post_wire(const void* data, size_t len) { send_message(data, len); }

    bool is_connected() const { return connected_.load(); }

    bool wait_for_close(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(close_mtx_);
        return close_cv_.wait_for(lk, timeout, [this]() {
            return close_received_.load() || !connected_.load();
        });
    }

    // Abort the recon socket WITHOUT joining: wakes any thread blocked in
    // send/recv on it (including a scanner session thread stuck writing
    // to a hung recon). Shutdown calls this before joining scanner
    // sessions; end_session()/stop() still do the join afterwards.
    void cancel() {
        connected_.store(false);
        shutdown_socket();
        close_cv_.notify_all();
    }

    // Called by marshal to stop everything
    void stop() { end_session(); }

private:
    std::string recon_host_;
    uint16_t recon_port_;
    uint32_t connect_timeout_ms_{5000};
    uint32_t user_timeout_ms_{kDefaultUserTimeoutMs};
    MessageCallback on_message_;
    FailureCallback on_failure_;

    net::io_context ioc_;
    std::unique_ptr<tcp::socket> socket_;
    std::mutex socket_mtx_;
    std::mutex send_mtx_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> reader_running_{false};
    std::atomic<bool> drop_logged_{false};
    std::atomic<bool> failure_reported_{false};
    std::atomic<bool> close_received_{false};
    std::atomic<uint64_t> session_epoch_{0};
    std::thread reader_;
    std::mutex close_mtx_;
    std::condition_variable close_cv_;

    void report_failure_once(uint64_t session_epoch) {
        if (failure_reported_.exchange(true) == false && on_failure_) {
            try { on_failure_(session_epoch); } catch (...) {}
        }
    }

    void shutdown_socket() {
        std::lock_guard<std::mutex> lk(socket_mtx_);
        if (socket_ && socket_->is_open()) {
            boost::system::error_code ec;
            socket_->shutdown(tcp::socket::shutdown_both, ec);
        }
    }

    void fail_recon(const std::string& reason, uint64_t session_epoch) {
        if (connected_.exchange(false)) {
            LOG_WARN(reason);
        } else if (drop_logged_.exchange(true) == false) {
            LOG_WARN(reason);
        }
        shutdown_socket();
        close_cv_.notify_all();
        report_failure_once(session_epoch);
    }

    // require_epoch: if non-zero, send only while that epoch owns the
    // connection (see post_close_for).
    void send_message(const void* data, size_t len, uint64_t require_epoch = 0) {
        if (!connected_.load()) {
            // Only log once to avoid spam during shutdown
            if (drop_logged_.exchange(true) == false)
                LOG_WARN("Not connected to recon, dropping messages");
            return;
        }

        std::lock_guard<std::mutex> lk(send_mtx_);
        if (!connected_.load()) return;
        const uint64_t epoch = session_epoch_.load();
        if (require_epoch != 0 && epoch != require_epoch) return;
        if (!write_exact(data, len)) {
            fail_recon("Write to recon failed", epoch);
            return;
        }
    }

    // Reader thread: reads MRD messages from recon and forwards them upstream.
    void read_loop(uint64_t session_epoch) {
        LOG_INFO("Recon reader started (epoch " << session_epoch << ")");
        bool failure = false;
        try {
            while (reader_running_.load() && connected_.load()) {
                uint16_t msg_id = 0;
                if (!read_exact(&msg_id, 2)) {
                    failure = reader_running_.load();
                    break;
                }

                std::vector<uint8_t> body;
                if (!read_message_body(msg_id, body)) {
                    failure = reader_running_.load();
                    break;
                }

                if (msg_id == MRD_MESSAGE_CLOSE) {
                    LOG_INFO("Recon sent CLOSE");
                    if (on_message_) {
                        try { on_message_(session_epoch, msg_id, body.data(), body.size()); } catch (...) {}
                    }
                    close_received_.store(true);
                    close_cv_.notify_all();
                    break;
                } else if (msg_id == MRD_MESSAGE_TEXT) {
                    LOG_DEBUG("Recon TEXT");
                } else {
                    LOG_DEBUG("Recon MRD message tag=" << msg_id
                              << " bytes=" << body.size());
                }

                if (on_message_) {
                    try { on_message_(session_epoch, msg_id, body.data(), body.size()); } catch (...) {}
                }
            }
        } catch (const std::exception& e) {
            LOG_WARN("Recon reader error: " << e.what());
            failure = true;
        }
        connected_.store(false);
        if (failure) shutdown_socket();
        close_cv_.notify_all();
        if (failure) report_failure_once(session_epoch);
        LOG_INFO("Recon reader ended");
    }

    bool read_exact(void* buf, size_t n) {
        auto* out = static_cast<uint8_t*>(buf);
        size_t done = 0;
        while (done < n) {
            const int fd = native_fd();
            if (fd < 0) return false;
            ssize_t rc = ::recv(fd, out + done, n - done, 0);
            if (rc == 0) return false;
            if (rc < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            done += static_cast<size_t>(rc);
        }
        return true;
    }

    bool write_exact(const void* buf, size_t n) {
        const auto* in = static_cast<const uint8_t*>(buf);
        size_t done = 0;
        while (done < n) {
            const int fd = native_fd();
            if (fd < 0) return false;
            ssize_t rc = ::send(fd, in + done, n - done, MSG_NOSIGNAL);
            if (rc < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (rc == 0) return false;
            done += static_cast<size_t>(rc);
        }
        return true;
    }

    int native_fd() {
        std::lock_guard<std::mutex> lk(socket_mtx_);
        if (!socket_ || !socket_->is_open()) return -1;
        return socket_->native_handle();
    }

    bool read_message_body(uint16_t tag, std::vector<uint8_t>& body) {
        switch (tag) {
        case MRD_MESSAGE_CONFIG_FILE:
            body.resize(1024);
            return read_exact(body.data(), body.size());

        case MRD_MESSAGE_CONFIG_TEXT:
        case MRD_MESSAGE_METADATA_XML_TEXT:
        case MRD_MESSAGE_TEXT:
            return read_len_prefixed_body(body);

        case MRD_MESSAGE_CLOSE:
            body.clear();
            return true;

        case MRD_MESSAGE_ISMRMRD_ACQUISITION:
            return read_acquisition_body(body);

        case MRD_MESSAGE_ISMRMRD_IMAGE:
            return read_image_body(body);

        case MRD_MESSAGE_ISMRMRD_WAVEFORM:
            return read_waveform_body(body);

        default:
            LOG_WARN("Unexpected MRD message from recon: " << tag);
            return false;
        }
    }

    bool read_len_prefixed_body(std::vector<uint8_t>& body) {
        uint32_t len = 0;
        if (!read_exact(&len, 4)) return false;
        // MEDIUM #12: cap the length so a hostile recon can't force a
        // multi-GB allocation before we've even typed the message.
        size_t body_size = 0;
        if (!validate_len_prefix_body(len, body_size)) {
            LOG_WARN("Recon len-prefixed body rejected: len "
                     << len << " exceeds cap");
            return false;
        }
        body.resize(body_size);
        std::memcpy(body.data(), &len, 4);
        if (len > 0 && !read_exact(body.data() + 4, len)) return false;
        return true;
    }

    bool read_acquisition_body(std::vector<uint8_t>& body) {
        ISMRMRD::AcquisitionHeader hdr;
        if (!read_exact(&hdr, ACQUISITION_HEADER_BYTES)) return false;

        size_t traj_bytes = 0, sample_bytes = 0;
        if (!compute_acquisition_payload_bytes(
                hdr.trajectory_dimensions, hdr.number_of_samples,
                hdr.active_channels, traj_bytes, sample_bytes)) {
            LOG_WARN("Recon ACQUISITION rejected: implausible size (traj_dims="
                     << hdr.trajectory_dimensions << " samples="
                     << hdr.number_of_samples << " channels="
                     << hdr.active_channels << ")");
            return false;
        }
        body.resize(ACQUISITION_HEADER_BYTES + traj_bytes + sample_bytes);
        std::memcpy(body.data(), &hdr, ACQUISITION_HEADER_BYTES);
        size_t off = ACQUISITION_HEADER_BYTES;
        if (traj_bytes > 0 && !read_exact(body.data() + off, traj_bytes)) return false;
        off += traj_bytes;
        if (sample_bytes > 0 && !read_exact(body.data() + off, sample_bytes)) return false;
        return true;
    }

    bool read_image_body(std::vector<uint8_t>& body) {
        std::vector<uint8_t> hdr_buf(IMAGE_HEADER_BYTES);
        if (!read_exact(hdr_buf.data(), IMAGE_HEADER_BYTES)) return false;

        const auto* ihdr = reinterpret_cast<const ISMRMRD::ImageHeader*>(hdr_buf.data());

        uint64_t attr_len_raw = 0;
        if (!read_exact(&attr_len_raw, 8)) return false;

        // HIGH #5/#7: validate sizes before any allocation. On recon side,
        // unlike the scanner path, the body is resized with `total` before
        // the attr read, so an overflowed `total` would produce a true OOB
        // write at read_exact into body.data() + off.
        size_t attr_len = 0;
        if (!validate_attr_len(attr_len_raw, attr_len)) {
            LOG_WARN("Recon image rejected: attr_len " << attr_len_raw
                     << " exceeds cap");
            return false;
        }
        size_t pixel_bytes = 0;
        if (!compute_pixel_bytes(ihdr->matrix_size[0],
                                 ihdr->matrix_size[1],
                                 ihdr->matrix_size[2],
                                 ihdr->channels,
                                 ISMRMRD::ismrmrd_sizeof_data_type(ihdr->data_type),
                                 pixel_bytes)) {
            LOG_WARN("Recon image rejected: invalid pixel-size product");
            return false;
        }
        size_t total = 0;
        if (!compute_image_body_total(IMAGE_HEADER_BYTES, attr_len,
                                       pixel_bytes, total)) {
            LOG_WARN("Recon image rejected: body total overflow or cap");
            return false;
        }

        uint64_t attr_len_wire = static_cast<uint64_t>(attr_len);
        body.resize(total);
        size_t off = 0;
        std::memcpy(body.data() + off, hdr_buf.data(), IMAGE_HEADER_BYTES); off += IMAGE_HEADER_BYTES;
        std::memcpy(body.data() + off, &attr_len_wire, 8); off += 8;
        if (attr_len > 0 && !read_exact(body.data() + off, attr_len)) return false;
        off += attr_len;
        if (pixel_bytes > 0 && !read_exact(body.data() + off, pixel_bytes)) return false;
        return true;
    }

    bool read_waveform_body(std::vector<uint8_t>& body) {
        ISMRMRD::WaveformHeader hdr;
        if (!read_exact(&hdr, WAVEFORM_HEADER_BYTES)) return false;
        size_t data_bytes = 0;
        if (!compute_waveform_data_bytes(hdr.number_of_samples, hdr.channels, data_bytes)) {
            LOG_WARN("Recon WAVEFORM rejected: implausible size (samples="
                     << hdr.number_of_samples << " channels=" << hdr.channels << ")");
            return false;
        }
        body.resize(WAVEFORM_HEADER_BYTES + data_bytes);
        std::memcpy(body.data(), &hdr, WAVEFORM_HEADER_BYTES);
        if (data_bytes > 0 && !read_exact(body.data() + WAVEFORM_HEADER_BYTES, data_bytes)) return false;
        return true;
    }
};

} // namespace mrd
