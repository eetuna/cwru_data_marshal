/*
 * ReconForwarder — forwards scanner MRD TCP messages to a reconstruction service.
 *
 * Design: simple TCP pipe, matching python-ismrmrd-server's connection model.
 * One socket per scan. Sequential writes. A reader thread reads MRD messages back.
 * No reconnect mid-scan. No queues. No complexity.
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
#include <cstdint>
#include <cstring>
#include <functional>
#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ismrmrd/ismrmrd.h>
#include <ismrmrd/waveform.h>

#include "mrd_stream_tags.hpp"
#include "logging.hpp"

namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace mrd {

class ReconForwarder {
public:
    using MessageCallback = std::function<void(uint16_t, const void*, size_t)>;
    using FailureCallback = std::function<void()>;

    ReconForwarder(const std::string& host, uint16_t port,
                   MessageCallback on_message, FailureCallback on_failure)
        : recon_host_(host), recon_port_(port),
          on_message_(std::move(on_message)), on_failure_(std::move(on_failure)) {}

    ~ReconForwarder() { end_session(); }

    // --- Session lifecycle (called from marshal's MRD TCP listener thread) ---

    // Connect to recon, start reader thread. Call ONCE per scan, before any send().
    bool begin_session() {
        end_session();  // clean up any previous session
        drop_logged_.store(false);
        close_received_.store(false);

        try {
            tcp::resolver resolver(ioc_);
            auto endpoints = resolver.resolve(recon_host_, std::to_string(recon_port_));
            socket_ = std::make_unique<tcp::socket>(ioc_);
            net::connect(*socket_, endpoints);
            socket_->set_option(tcp::no_delay(true));
            connected_.store(true);
            LOG_INFO("Connected to recon at " << recon_host_ << ":" << recon_port_);

            // Start reader thread for recon responses.
            reader_running_.store(true);
            reader_ = std::thread(&ReconForwarder::read_loop, this);
            return true;
        } catch (const std::exception& e) {
            LOG_WARN("Failed to connect to recon: " << e.what());
            connected_.store(false);
            if (on_failure_) { try { on_failure_(); } catch (...) {} }
            return false;
        }
    }

    // Close connection, join reader thread. Call after CLOSE is sent.
    void end_session() {
        reader_running_.store(false);

        if (socket_ && socket_->is_open()) {
            boost::system::error_code ec;
            socket_->shutdown(tcp::socket::shutdown_both, ec);
            socket_->close(ec);
        }
        socket_.reset();
        connected_.store(false);

        if (reader_.joinable()) reader_.join();
    }

    // --- Message sending (called from marshal's MRD TCP listener thread) ---
    // All of these write directly to the socket. No queue. Sequential.

    void send_config_file(const std::string& name) {
        uint8_t buf[2 + 1024] = {};
        uint16_t tag = MRD_MESSAGE_CONFIG_FILE;
        std::memcpy(buf, &tag, 2);
        std::memcpy(buf + 2, name.data(), std::min(name.size(), size_t(1024)));
        write_all(buf, sizeof(buf));
    }

    void send_config_text(const std::string& text) {
        std::string with_nul = text + '\0';
        uint32_t len = static_cast<uint32_t>(with_nul.size());
        std::vector<uint8_t> msg(2 + 4 + len);
        uint16_t tag = MRD_MESSAGE_CONFIG_TEXT;
        std::memcpy(msg.data(), &tag, 2);
        std::memcpy(msg.data() + 2, &len, 4);
        std::memcpy(msg.data() + 6, with_nul.data(), len);
        write_all(msg.data(), msg.size());
    }

    void send_header(const std::string& xml) {
        std::string with_nul = xml + '\0';
        uint32_t len = static_cast<uint32_t>(with_nul.size());
        std::vector<uint8_t> msg(2 + 4 + len);
        uint16_t tag = MRD_MESSAGE_METADATA_XML_TEXT;
        std::memcpy(msg.data(), &tag, 2);
        std::memcpy(msg.data() + 2, &len, 4);
        std::memcpy(msg.data() + 6, with_nul.data(), len);
        write_all(msg.data(), msg.size());
    }

    void send_frame(uint16_t tag, const void* data, size_t len) {
        std::vector<uint8_t> msg(2 + len);
        std::memcpy(msg.data(), &tag, 2);
        std::memcpy(msg.data() + 2, data, len);
        write_all(msg.data(), msg.size());
    }

    void send_close() {
        uint16_t tag = MRD_MESSAGE_CLOSE;
        write_all(&tag, 2);
    }

    // Legacy API names (used by mrd_tcp_listener)
    void post_header(const std::string& xml)    { send_header(xml); }
    void post_config(const std::string& config) { send_config_file(config); }
    void post_config_text(const std::string& t) { send_config_text(t); }
    void post_frame(uint16_t tag, const std::string& body) {
        send_frame(tag, body.data(), body.size());
    }
    void post_close() { send_close(); }

    bool is_connected() const { return connected_.load(); }

    bool wait_for_close(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(close_mtx_);
        return close_cv_.wait_for(lk, timeout, [this]() {
            return close_received_.load() || !connected_.load();
        });
    }

    // Called by marshal to stop everything
    void stop() { end_session(); }

private:
    std::string recon_host_;
    uint16_t recon_port_;
    MessageCallback on_message_;
    FailureCallback on_failure_;

    net::io_context ioc_;
    std::unique_ptr<tcp::socket> socket_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> reader_running_{false};
    std::atomic<bool> drop_logged_{false};
    std::atomic<bool> close_received_{false};
    std::thread reader_;
    std::mutex close_mtx_;
    std::condition_variable close_cv_;

    void write_all(const void* data, size_t len) {
        if (!connected_.load() || !socket_ || !socket_->is_open()) {
            // Only log once to avoid spam during shutdown
            if (drop_logged_.exchange(true) == false)
                LOG_WARN("Not connected to recon, dropping messages");
            return;
        }
        try {
            net::write(*socket_, net::buffer(data, len));
        } catch (const std::exception& e) {
            LOG_WARN("Write to recon failed: " << e.what());
            connected_.store(false);
            if (on_failure_) { try { on_failure_(); } catch (...) {} }
        }
    }

    // Reader thread: reads MRD messages from recon and forwards them upstream.
    void read_loop() {
        LOG_INFO("Recon reader started");
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
                        try { on_message_(msg_id, body.data(), body.size()); } catch (...) {}
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
                    try { on_message_(msg_id, body.data(), body.size()); } catch (...) {}
                }
            }
        } catch (const std::exception& e) {
            LOG_WARN("Recon reader error: " << e.what());
            failure = true;
        }
        connected_.store(false);
        close_cv_.notify_all();
        if (failure && on_failure_) { try { on_failure_(); } catch (...) {} }
        LOG_INFO("Recon reader ended");
    }

    bool read_exact(void* buf, size_t n) {
        if (!socket_ || !socket_->is_open()) return false;
        boost::system::error_code ec;
        net::read(*socket_, net::buffer(buf, n), ec);
        return !ec;
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
        body.resize(4 + len);
        std::memcpy(body.data(), &len, 4);
        if (len > 0 && !read_exact(body.data() + 4, len)) return false;
        return true;
    }

    bool read_acquisition_body(std::vector<uint8_t>& body) {
        ISMRMRD::AcquisitionHeader hdr;
        if (!read_exact(&hdr, ACQUISITION_HEADER_BYTES)) return false;

        const size_t traj_bytes = size_t(hdr.trajectory_dimensions)
                                * hdr.number_of_samples * sizeof(float);
        const size_t sample_bytes = size_t(hdr.number_of_samples)
                                  * hdr.active_channels * sizeof(complex_float_t);
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

        uint64_t attr_len = 0;
        if (!read_exact(&attr_len, 8)) return false;

        size_t npixels = static_cast<size_t>(ihdr->matrix_size[0])
                       * ihdr->matrix_size[1]
                       * std::max<uint16_t>(ihdr->matrix_size[2], 1)
                       * std::max<uint16_t>(ihdr->channels, 1);
        size_t pixel_bytes = npixels * ISMRMRD::ismrmrd_sizeof_data_type(ihdr->data_type);
        size_t total = IMAGE_HEADER_BYTES + 8 + attr_len + pixel_bytes;
        body.resize(total);
        size_t off = 0;
        std::memcpy(body.data() + off, hdr_buf.data(), IMAGE_HEADER_BYTES); off += IMAGE_HEADER_BYTES;
        std::memcpy(body.data() + off, &attr_len, 8); off += 8;
        if (attr_len > 0 && !read_exact(body.data() + off, attr_len)) return false;
        off += attr_len;
        if (pixel_bytes > 0 && !read_exact(body.data() + off, pixel_bytes)) return false;
        return true;
    }

    bool read_waveform_body(std::vector<uint8_t>& body) {
        ISMRMRD::WaveformHeader hdr;
        if (!read_exact(&hdr, WAVEFORM_HEADER_BYTES)) return false;
        const size_t data_bytes = size_t(hdr.number_of_samples) * hdr.channels * sizeof(uint32_t);
        body.resize(WAVEFORM_HEADER_BYTES + data_bytes);
        std::memcpy(body.data(), &hdr, WAVEFORM_HEADER_BYTES);
        if (data_bytes > 0 && !read_exact(body.data() + WAVEFORM_HEADER_BYTES, data_bytes)) return false;
        return true;
    }
};

} // namespace mrd
