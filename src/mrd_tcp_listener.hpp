/*
 * File: src/mrd_tcp_listener.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Raw TCP MRD listener — a transparent stand-in for the recon server.
 *
 * The scanner connects exactly as it would to python-ismrmrd-server
 * (connection.py / constants.py) and the marshal behaves like that server:
 *
 *   - one scan per connection; connections are served one at a time, a
 *     second one waits its turn (like the server's accept backlog)
 *   - on connect, the marshal opens its own connection to recon
 *   - every scanner message is archived and forwarded to recon, verbatim
 *     and in order; every recon message is relayed back to the scanner
 *     (recon_forwarder.hpp reader thread + push_message_to_scanner)
 *   - the scanner's CLOSE is forwarded; the marshal then waits — with no
 *     time limit — until recon sends its CLOSE or its connection ends,
 *     relays it, and closes the scanner connection (as the server does)
 *
 * Nothing here is driven by a clock: a scan ends when the messages say so
 * or a connection ends. If the recon connection fails, the scanner gets a
 * "RECON FAILED" image and a CLOSE, so the scan still finalizes.
 */

#pragma once

#undef LOG_COMPONENT
#define LOG_COMPONENT "mrd_tcp"
#include "logging.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <boost/asio.hpp>

#include <ismrmrd/ismrmrd.h>
#include <ismrmrd/waveform.h>

#include "live_image_store.hpp"
#include "marshal_state.hpp"
#include "mrd_io.hpp"
#include "mrd_stream_tags.hpp"
#include "recon_failure_image.hpp"
#include "recon_forwarder.hpp"
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
        // GET /status hook. Safe lifetime: main() joins all HTTP sessions
        // before this listener is destroyed.
        state_.mrd_scanner_connected = [this] { return has_scanner(); };
        worker_ = std::thread([this] { serve(); });
        LOG_INFO("MRD TCP listener on port " << port);
        do_accept();
    }

    ~MrdTcpListener() { stop(); }

    // Graceful shutdown: stop accepting, close the scanner and recon
    // connections (which wakes any blocked read or wait), join the worker.
    void stop() {
        {
            std::lock_guard<std::mutex> lk(scanner_mtx_);
            if (stopping_ && !worker_.joinable()) return;
            stopping_ = true;
            for (auto& s : pending_) close_socket(*s);
            pending_.clear();
            if (scanner_socket_) close_socket(*scanner_socket_);
        }
        pending_cv_.notify_all();
        boost::system::error_code ignore;
        acceptor_.close(ignore);
        if (forwarder_) forwarder_->end_session();
        if (worker_.joinable()) worker_.join();
    }

    // Push one MRD message to the scanner. Serialized so a frame (tag + body)
    // is never interleaved with another; blocking send = TCP flow control,
    // the same backpressure a direct scanner<->recon connection has.
    // Returns true when written to a connected scanner.
    bool push_message_to_scanner(uint16_t tag, const void* data, size_t len) {
        std::lock_guard<std::mutex> send_lk(scanner_send_mtx_);
        std::shared_ptr<tcp::socket> sock;
        {
            std::lock_guard<std::mutex> lk(scanner_mtx_);
            if (scanner_socket_ && scanner_socket_->is_open()) sock = scanner_socket_;
        }
        if (!sock) {
            LOG_DEBUG("Scanner writer: no scanner connected, drop tag=" << tag);
            return false;
        }
        const std::array<net::const_buffer, 2> frame{
            net::buffer(&tag, sizeof(tag)), net::buffer(data, len)};
        boost::system::error_code ec;
        net::write(*sock, frame, ec);
        if (ec) {
            if (!is_stopping()) LOG_WARN("Scanner writer: failed to push MRD message tag=" << tag);
            close_socket(*sock);   // the session's read fails and it unwinds
            return false;
        }
        if (tag == MRD_MESSAGE_CLOSE) close_sent_.store(true);
        return true;
    }

    bool has_scanner() const {
        std::lock_guard<std::mutex> lk(scanner_mtx_);
        return scanner_socket_ && scanner_socket_->is_open();
    }

private:
    tcp::acceptor acceptor_;
    MarshalState& state_;
    ReconForwarder* forwarder_;

    mutable std::mutex scanner_mtx_;          // scanner_socket_, pending_, stopping_
    std::condition_variable pending_cv_;
    std::deque<std::shared_ptr<tcp::socket>> pending_;
    std::shared_ptr<tcp::socket> scanner_socket_;
    bool stopping_{false};
    std::mutex scanner_send_mtx_;             // order: scanner_send_mtx_ before scanner_mtx_
    std::atomic<bool> close_sent_{false};     // a CLOSE reached the scanner this session
    std::thread worker_;

    bool is_stopping() const {
        std::lock_guard<std::mutex> lk(scanner_mtx_);
        return stopping_;
    }

    static void close_socket(tcp::socket& s) {
        boost::system::error_code ignore;
        s.shutdown(tcp::socket::shutdown_both, ignore);
        s.close(ignore);
    }

    // remote_endpoint() throws if the peer already left; use the ec overload.
    static std::string peer_string(const tcp::socket& s) {
        boost::system::error_code ec;
        auto ep = s.remote_endpoint(ec);
        if (ec) return "<unknown>";
        std::ostringstream oss;
        oss << ep;
        return oss.str();
    }

    // Dead-peer detection on the scanner socket: keepalive catches a
    // scanner that vanished while idle; TCP_USER_TIMEOUT catches one that
    // vanished while images to it were unacknowledged. Both only fire when
    // the scanner host stops answering at the TCP level.
    static void configure_scanner_socket(tcp::socket& s) {
        boost::system::error_code ec;
        s.set_option(tcp::no_delay(true), ec);
        s.set_option(net::socket_base::keep_alive(true), ec);
        const int fd = s.native_handle();
        int v = 30;
        ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &v, sizeof(v));
        v = 10;
        ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &v, sizeof(v));
        v = 3;
        ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &v, sizeof(v));
        unsigned ut = 60000;
        ::setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &ut, sizeof(ut));
    }

    void do_accept() {
        acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket sock) {
            if (!ec) {
                auto s = std::make_shared<tcp::socket>(std::move(sock));
                bool queued = false;
                size_t waiting = 0;
                {
                    std::lock_guard<std::mutex> lk(scanner_mtx_);
                    if (!stopping_) {
                        pending_.push_back(s);
                        queued = true;
                        waiting = pending_.size() - (scanner_socket_ ? 0 : 1);
                    }
                }
                if (!queued) {
                    close_socket(*s);
                } else {
                    if (waiting > 0)
                        LOG_INFO("Scanner connection from " << peer_string(*s)
                                 << " waits for the current scan to finish");
                    pending_cv_.notify_one();
                }
            }
            if (!is_stopping() && acceptor_.is_open()) do_accept();
        });
    }

    // Serve queued scanner connections one at a time.
    void serve() {
        for (;;) {
            std::shared_ptr<tcp::socket> sock;
            {
                std::unique_lock<std::mutex> lk(scanner_mtx_);
                pending_cv_.wait(lk, [this] { return stopping_ || !pending_.empty(); });
                if (stopping_) return;
                sock = pending_.front();
                pending_.pop_front();
                scanner_socket_ = sock;
            }
            try {
                handle_session(sock);
            } catch (const std::exception& e) {
                LOG_WARN("MRD session error: " << e.what());
            }
            close_socket(*sock);
            {
                std::lock_guard<std::mutex> lk(scanner_mtx_);
                scanner_socket_.reset();
            }
        }
    }

    // Buffered reader: one recv takes whatever the kernel has; messages are
    // parsed out of it contiguously ([tag][body]) so they can be forwarded
    // verbatim. ensure(n) returns as soon as n bytes are there (never delays).
    class StreamReader {
    public:
        explicit StreamReader(tcp::socket& s) : sock_(s), buf_(1 << 18) {}

        bool ensure(size_t n) {
            if (avail() >= n) return true;
            if (n > buf_.size()) {
                std::vector<uint8_t> bigger(((n + 4095) / 4096) * 4096);
                std::memcpy(bigger.data(), buf_.data() + head_, avail());
                tail_ = avail();
                head_ = 0;
                buf_.swap(bigger);
            } else if (head_ + n > buf_.size()) {
                std::memmove(buf_.data(), buf_.data() + head_, avail());
                tail_ = avail();
                head_ = 0;
            }
            while (avail() < n) {
                boost::system::error_code ec;
                const size_t got = sock_.read_some(
                    net::buffer(buf_.data() + tail_, buf_.size() - tail_), ec);
                if (ec || got == 0) return false;
                tail_ += got;
            }
            return true;
        }
        const uint8_t* cur() const { return buf_.data() + head_; }
        void consume(size_t n) {
            head_ += n;
            if (head_ == tail_) { head_ = 0; tail_ = 0; }
        }

    private:
        size_t avail() const { return tail_ - head_; }
        tcp::socket& sock_;
        std::vector<uint8_t> buf_;
        size_t head_{0};
        size_t tail_{0};
    };

    void archive(const char* what, DumpEnqueueResult r) {
        if (r == DumpEnqueueResult::Dropped)
            LOG_WARN("Archive dropped a record (" << what << "): disk not keeping up");
    }

    void handle_session(const std::shared_ptr<tcp::socket>& sock) {
        configure_scanner_socket(*sock);
        LOG_INFO("Scanner connected from " << peer_string(*sock));
        close_sent_.store(false);
        state_.set_status("scanning");

        const std::string recon_target = state_.recon_url;
        const bool recon_up = forwarder_ && forwarder_->begin_session();
        if (!recon_up) {
            LOG_WARN("Recon unreachable at " << recon_target
                     << " — the scan is archived; the scanner gets RECON FAILED");
            state_.set_status("recon failed", "cannot connect to recon at " + recon_target);
        }

        std::optional<ISMRMRD::AcquisitionHeader> last_acq;   // geometry for the failure image
        bool failure_image_sent = false;
        auto recon_failed = [&] { return !recon_up || forwarder_->failed(); };
        // Tell the scanner once, as soon as the failure is known and the
        // scan's geometry is (raw data seen), or at the end of the scan.
        auto report_failure = [&](bool at_end) {
            if (failure_image_sent || !recon_failed()) return;
            if (!last_acq && !at_end) return;
            failure_image_sent = true;
            auto body = build_recon_failure_image_body(last_acq ? &*last_acq : nullptr);
            push_message_to_scanner(MRD_MESSAGE_ISMRMRD_IMAGE, body.data(), body.size());
            if (recon_up && forwarder_->failed())
                state_.set_status("recon failed", "recon connection ended without CLOSE");
        };
        // Forward one complete wire message ([tag][body]) to recon.
        auto forward = [&](const uint8_t* wire, size_t len) {
            if (recon_up) forwarder_->post_wire(wire, len);
        };

        auto& dump = *state_.dump_recorder;
        bool scanner_closed = false;
        StreamReader rd(*sock);

        while (true) {
            if (!rd.ensure(2)) break;
            uint16_t msg_id = 0;
            std::memcpy(&msg_id, rd.cur(), 2);

            if (msg_id == MRD_MESSAGE_CLOSE) {
                rd.consume(2);
                LOG_INFO("CLOSE");
                scanner_closed = true;
                break;
            }

            size_t total = 0;   // whole wire message, tag included
            switch (msg_id) {

            case MRD_MESSAGE_CONFIG_FILE: {
                total = 2 + 1024;
                if (!rd.ensure(total)) goto done;
                const char* name = reinterpret_cast<const char*>(rd.cur() + 2);
                std::string config(name, strnlen(name, 1024));
                LOG_INFO("CONFIG_FILE: " << config);
                {
                    std::lock_guard<std::mutex> lk(state_.scan_mtx);
                    state_.current_config = config;
                    state_.config_received.store(true);
                }
                archive("config_file", dump.set_scanner_config_file(
                    std::vector<uint8_t>(rd.cur() + 2, rd.cur() + total)));
                break;
            }

            case MRD_MESSAGE_CONFIG_TEXT:
            case MRD_MESSAGE_METADATA_XML_TEXT:
            case MRD_MESSAGE_TEXT: {
                if (!rd.ensure(2 + 4)) goto done;
                uint32_t len = 0;
                std::memcpy(&len, rd.cur() + 2, 4);
                size_t body_size = 0;
                if (!validate_len_prefix_body(len, body_size)) {
                    LOG_WARN("Message " << msg_id << " rejected: length " << len << " exceeds cap");
                    goto done;
                }
                total = 2 + body_size;
                if (!rd.ensure(total)) goto done;
                std::vector<uint8_t> body(rd.cur() + 2, rd.cur() + total);
                std::string text(reinterpret_cast<const char*>(body.data() + 4), len);
                if (auto nul = text.find('\0'); nul != std::string::npos) text.resize(nul);

                if (msg_id == MRD_MESSAGE_CONFIG_TEXT) {
                    LOG_INFO("CONFIG_TEXT: " << text);
                    {
                        std::lock_guard<std::mutex> lk(state_.scan_mtx);
                        state_.current_config = text;
                        state_.config_received.store(true);
                    }
                    archive("config_text", dump.set_scanner_config_text(std::move(body)));
                } else if (msg_id == MRD_MESSAGE_TEXT) {
                    LOG_INFO("TEXT: " << text);
                    archive("scanner_text", dump.append_scanner_text(std::move(body)));
                } else {
                    LOG_INFO("METADATA_XML: " << text.size() << " bytes");
                    std::string scan_file;
                    {
                        std::lock_guard<std::mutex> lk(state_.scan_mtx);
                        if (state_.current_scan_filename.empty())
                            state_.current_scan_filename = scan_filename();
                        scan_file = state_.current_scan_filename;
                        state_.current_xml_header = text;
                    }
                    {
                        // Geometry belongs to a scan's slice prescription.
                        std::lock_guard<std::mutex> lk(state_.slice_geom_mtx);
                        state_.slice_geom.clear();
                        state_.latest_slice = -1;
                    }
                    // A new scan starts from zero, never from where the
                    // previous scan's slice was driven to.
                    state_.reset_slice_state();
                    state_.snapshot->begin_scan(text);
                    archive("start_scan", dump.start_scan(scan_file, std::move(body), text));
                    state_.header_received.store(true);
                }
                break;
            }

            case MRD_MESSAGE_ISMRMRD_ACQUISITION: {
                if (!rd.ensure(2 + ACQUISITION_HEADER_BYTES)) goto done;
                ISMRMRD::AcquisitionHeader ahdr;
                std::memcpy(&ahdr, rd.cur() + 2, ACQUISITION_HEADER_BYTES);
                const size_t traj_bytes = size_t(ahdr.trajectory_dimensions)
                                        * ahdr.number_of_samples * sizeof(float);
                const size_t sample_bytes = size_t(ahdr.number_of_samples)
                                          * ahdr.active_channels * sizeof(complex_float_t);
                total = 2 + ACQUISITION_HEADER_BYTES + traj_bytes + sample_bytes;
                if (total > kMaxImageFrameBytes) {
                    LOG_WARN("ACQUISITION rejected: implausible size " << total);
                    goto done;
                }
                if (!rd.ensure(total)) goto done;
                const uint8_t* p = rd.cur() + 2 + ACQUISITION_HEADER_BYTES;
                archive("scanner_acquisition", dump.append_scanner_acquisition(
                    ahdr, std::vector<uint8_t>(p, p + traj_bytes),
                    std::vector<uint8_t>(p + traj_bytes, p + traj_bytes + sample_bytes)));
                last_acq = ahdr;
                break;
            }

            case MRD_MESSAGE_ISMRMRD_IMAGE: {
                if (!rd.ensure(2 + IMAGE_HEADER_BYTES + 8)) goto done;
                ISMRMRD::ImageHeader ihdr;
                std::memcpy(&ihdr, rd.cur() + 2, IMAGE_HEADER_BYTES);
                uint64_t attr_len_raw = 0;
                std::memcpy(&attr_len_raw, rd.cur() + 2 + IMAGE_HEADER_BYTES, 8);
                size_t attr_len = 0, pixel_bytes = 0, body_size = 0;
                if (!validate_attr_len(attr_len_raw, attr_len) ||
                    !compute_pixel_bytes(ihdr.matrix_size[0], ihdr.matrix_size[1],
                                         ihdr.matrix_size[2], ihdr.channels,
                                         ISMRMRD::ismrmrd_sizeof_data_type(ihdr.data_type),
                                         pixel_bytes) ||
                    !compute_image_body_total(IMAGE_HEADER_BYTES, attr_len, pixel_bytes, body_size)) {
                    LOG_WARN("IMAGE rejected: invalid sizes");
                    goto done;
                }
                total = 2 + body_size;
                if (!rd.ensure(total)) goto done;
                const uint8_t* body = rd.cur() + 2;
                update_slice_geometry(state_, body, body_size);
                state_.perf_scanner_images_received.fetch_add(1, std::memory_order_relaxed);
                const uint8_t* attr_p = body + IMAGE_HEADER_BYTES + 8;
                archive("scanner_image", dump.append_scanner_image(
                    ihdr, std::vector<uint8_t>(attr_p, attr_p + attr_len),
                    std::vector<uint8_t>(attr_p + attr_len, attr_p + attr_len + pixel_bytes)));
                break;
            }

            case MRD_MESSAGE_ISMRMRD_WAVEFORM: {
                if (!rd.ensure(2 + WAVEFORM_HEADER_BYTES)) goto done;
                ISMRMRD::WaveformHeader whdr;
                std::memcpy(&whdr, rd.cur() + 2, WAVEFORM_HEADER_BYTES);
                const size_t data_bytes = size_t(whdr.number_of_samples) * whdr.channels * sizeof(uint32_t);
                total = 2 + WAVEFORM_HEADER_BYTES + data_bytes;
                if (!rd.ensure(total)) goto done;
                const uint8_t* d = rd.cur() + 2 + WAVEFORM_HEADER_BYTES;
                state_.perf_scanner_waveforms_received.fetch_add(1, std::memory_order_relaxed);
                archive("scanner_waveform", dump.append_scanner_waveform(
                    whdr, std::vector<uint8_t>(d, d + data_bytes)));
                break;
            }

            default:
                // MRD messages carry no length field, so an unknown type
                // cannot be skipped or forwarded; python-ismrmrd-server
                // also ends the connection here.
                LOG_WARN("Unknown MRD message ID " << msg_id << " — ending the connection");
                goto done;
            }

            forward(rd.cur(), total);
            rd.consume(total);
            report_failure(false);
        }

    done:
        // End of the scan: tell recon (if the scanner did not end it
        // properly, recon still gets a CLOSE so it finishes and sends what it
        // has), then wait until recon is done. No time limit.
        if (recon_up) {
            if (!forwarder_->close_received()) {
                const uint16_t close_tag = MRD_MESSAGE_CLOSE;
                forwarder_->post_wire(&close_tag, sizeof(close_tag));
            }
            state_.set_status("waiting for recon to finish");
            forwarder_->wait_until_done();
            forwarder_->end_session();
        }
        if (scanner_closed) {
            report_failure(true);
            if (!close_sent_.load()) push_message_to_scanner(MRD_MESSAGE_CLOSE, nullptr, 0);
        } else {
            LOG_WARN("Scanner connection ended without CLOSE");
        }

        if (recon_failed()) {
            if (recon_up) state_.set_status("recon failed", "recon connection ended without CLOSE");
        } else {
            state_.set_status("scan finished");
        }
        state_.close_scan();   // archive conversion continues in the background
        LOG_INFO("MRD session ended");
    }
};

} // namespace mrd
