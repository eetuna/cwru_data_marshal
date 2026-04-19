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
        writer_thread_ = std::thread(&MrdTcpListener::writer_loop, this);
        do_accept();
    }

    ~MrdTcpListener() {
        stop();
    }

    // Called on graceful shutdown. Closes the acceptor + any active scanner
    // socket, then joins the session thread. Safe to call multiple times.
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

        // Stop the async writer thread.
        {
            std::lock_guard<std::mutex> lk(writer_mtx_);
            writer_stopping_ = true;
        }
        writer_cv_.notify_all();
        if (writer_thread_.joinable()) writer_thread_.join();
    }

    // Enqueue a recon MRD message to push back to the scanner. Non-blocking;
    // the actual blocking ::send runs on writer_thread_.
    // HIGH #4: caller (recon reader) used to block on ::send here, which could
    // deadlock shutdown if the scanner socket was stuck.
    void push_message_to_scanner(uint16_t tag, const void* data, size_t len) {
        std::vector<uint8_t> payload;
        if (len > 0) {
            payload.assign(static_cast<const uint8_t*>(data),
                           static_cast<const uint8_t*>(data) + len);
        }
        std::lock_guard<std::mutex> lk(writer_mtx_);
        if (writer_stopping_) return;
        // Bounded: drop oldest if over cap. Log once.
        if (writer_queue_.size() >= kWriterQueueMax) {
            writer_queue_.pop_front();
            if (!writer_drop_logged_.exchange(true)) {
                LOG_WARN("Scanner writer queue full (" << kWriterQueueMax
                         << "); dropping oldest frames");
            }
        }
        writer_queue_.push_back({tag, std::move(payload)});
        writer_cv_.notify_one();
    }

    bool has_scanner() const {
        std::lock_guard<std::mutex> lk(scanner_mtx_);
        return scanner_socket_ && scanner_socket_->is_open();
    }

private:
    static constexpr size_t kWriterQueueMax = 1024;

    struct WriterJob {
        uint16_t tag;
        std::vector<uint8_t> payload;  // empty if no body
    };

    tcp::acceptor acceptor_;
    MarshalState& state_;
    ReconForwarder* forwarder_;
    mutable std::mutex scanner_mtx_;
    std::shared_ptr<tcp::socket> scanner_socket_;
    std::atomic<bool> session_active_{false};
    bool stopping_{false};           // protected by scanner_mtx_
    SessionRegistry sessions_;

    // Async writer: decouples recon-reader from blocking ::send to scanner.
    std::mutex writer_mtx_;
    std::condition_variable writer_cv_;
    std::deque<WriterJob> writer_queue_;
    bool writer_stopping_{false};
    std::atomic<bool> writer_drop_logged_{false};
    std::thread writer_thread_;

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

    void writer_loop() {
        while (true) {
            WriterJob job;
            {
                std::unique_lock<std::mutex> lk(writer_mtx_);
                writer_cv_.wait(lk, [this] {
                    return writer_stopping_ || !writer_queue_.empty();
                });
                if (writer_queue_.empty()) {
                    // Woken by stop with no work.
                    if (writer_stopping_) return;
                    continue;
                }
                job = std::move(writer_queue_.front());
                writer_queue_.pop_front();
            }

            // Grab the fd under scanner_mtx_, but release before the blocking
            // ::send so other callers aren't serialized on it.
            int fd = -1;
            std::shared_ptr<tcp::socket> sock_hold;
            {
                std::lock_guard<std::mutex> lk(scanner_mtx_);
                if (scanner_socket_ && scanner_socket_->is_open()) {
                    fd = scanner_socket_->native_handle();
                    sock_hold = scanner_socket_;  // keep alive while we send
                }
            }
            if (fd < 0) {
                LOG_DEBUG("Scanner writer: no scanner connected, drop tag=" << job.tag);
                continue;
            }
            if (!write_exact_fd(fd, &job.tag, sizeof(job.tag)) ||
                (!job.payload.empty() &&
                 !write_exact_fd(fd, job.payload.data(), job.payload.size()))) {
                LOG_WARN("Scanner writer: failed to push MRD message tag=" << job.tag);
                // Socket is broken; close it so the session exits.
                boost::system::error_code ignore;
                if (sock_hold) {
                    sock_hold->shutdown(tcp::socket::shutdown_both, ignore);
                    sock_hold->close(ignore);
                }
            } else {
                LOG_DEBUG("Scanner writer: pushed tag=" << job.tag
                          << " (" << job.payload.size() << " bytes)");
            }
        }
    }

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

        auto send_or_buffer_recon = [&](uint16_t tag, const std::vector<uint8_t>& body) {
            if (!forwarder_) return;
            if (session_active_.load() && forwarder_->is_connected()) {
                forwarder_->post_frame(tag, body);
            } else {
                recon_preamble.emplace_back(tag, body);
            }
        };

        auto ensure_recon_session = [&]() -> bool {
            if (!forwarder_) return false;
            if (!session_active_.load() || !forwarder_->is_connected()) {
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
                        check_dump_result("config_file",
                            state_.dump_recorder->set_scanner_config_file(config));
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
                        check_dump_result("config_text",
                            state_.dump_recorder->set_scanner_config_text(config));
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
                        if (state_.current_scan_filename.empty())
                            state_.current_scan_filename = scan_filename();
                        scan_file = state_.current_scan_filename;
                        state_.current_xml_header = xml;
                        state_.recon_expected_slices = nz;
                    }
                    if (state_.dump_enabled && state_.dump_recorder) {
                        check_dump_result("start_scan",
                            state_.dump_recorder->start_scan(scan_file, xml));
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
                        check_dump_result("scanner_text",
                            state_.dump_recorder->append_scanner_text(text));
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
        if (forwarder_ && session_active_.load()) {
            forwarder_->end_session();
        }
        flush_all_live_lanes(state_);
        state_.close_scan();
        LOG_INFO("MRD session ended");
        session_active_.store(false);
        {
            std::lock_guard<std::mutex> lk(scanner_mtx_);
            scanner_socket_.reset();
        }
    }
};

} // namespace mrd
