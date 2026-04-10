/*
 * File: src/recon_forwarder.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Forward scanner data to recon via MRD TCP, read images back
 *
 * Opens a TCP connection to the recon service (python-ismrmrd-server).
 * Writer thread sends MRD messages. Reader thread receives images back.
 * Full-duplex: reader and writer use the socket concurrently (no shared mutex).
 * On failure: drops messages, calls failure callback (marshal keeps running).
 */

#pragma once

#undef LOG_COMPONENT
#define LOG_COMPONENT "recon_fwd"
#include "logging.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <ismrmrd/ismrmrd.h>

#include "mrd_stream_tags.hpp"

namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace mrd {

class ReconForwarder {
public:
    using FailureCallback = std::function<void()>;
    using ImageCallback = std::function<void(const void*, size_t)>;

    ReconForwarder(const std::string& recon_host, uint16_t recon_port,
                   ImageCallback on_image = nullptr,
                   FailureCallback on_failure = nullptr)
        : recon_host_(recon_host)
        , recon_port_(recon_port)
        , on_image_(std::move(on_image))
        , on_failure_(std::move(on_failure))
    {
        writer_ = std::thread(&ReconForwarder::write_loop, this);
    }

    ~ReconForwarder() { stop(); }

    ReconForwarder(const ReconForwarder&) = delete;
    ReconForwarder& operator=(const ReconForwarder&) = delete;

    // Enqueue a pre-built MRD message (tag + body already assembled)
    void send_raw(std::vector<uint8_t> msg) { enqueue(std::move(msg)); }

    // Build and enqueue CONFIG_FILE (tag 1, 1024 bytes fixed)
    void send_config_file(const std::string& name) {
        std::vector<uint8_t> msg(2 + 1024, 0);
        uint16_t tag = MRD_MESSAGE_CONFIG_FILE;
        std::memcpy(msg.data(), &tag, 2);
        std::memcpy(msg.data() + 2, name.data(), std::min(name.size(), size_t(1024)));
        enqueue(std::move(msg));
    }

    // Build and enqueue CONFIG_TEXT (tag 2)
    void send_config_text(const std::string& text) {
        std::string with_nul = text + '\0';
        uint32_t len = static_cast<uint32_t>(with_nul.size());
        std::vector<uint8_t> msg(2 + 4 + len);
        uint16_t tag = MRD_MESSAGE_CONFIG_TEXT;
        std::memcpy(msg.data(), &tag, 2);
        std::memcpy(msg.data() + 2, &len, 4);
        std::memcpy(msg.data() + 6, with_nul.data(), len);
        enqueue(std::move(msg));
    }

    // Build and enqueue METADATA_XML (tag 3)
    void send_header(const std::string& xml) {
        std::string with_nul = xml + '\0';
        uint32_t len = static_cast<uint32_t>(with_nul.size());
        std::vector<uint8_t> msg(2 + 4 + len);
        uint16_t tag = MRD_MESSAGE_METADATA_XML_TEXT;
        std::memcpy(msg.data(), &tag, 2);
        std::memcpy(msg.data() + 2, &len, 4);
        std::memcpy(msg.data() + 6, with_nul.data(), len);
        enqueue(std::move(msg));
    }

    // Build and enqueue a typed frame. Caller must specify the tag.
    void send_typed_frame(uint16_t tag, const void* data, size_t len) {
        std::vector<uint8_t> msg(2 + len);
        std::memcpy(msg.data(), &tag, 2);
        std::memcpy(msg.data() + 2, data, len);
        enqueue(std::move(msg));
    }

    // Build and enqueue CLOSE (tag 4)
    void send_close() {
        std::vector<uint8_t> msg(2);
        uint16_t tag = MRD_MESSAGE_CLOSE;
        std::memcpy(msg.data(), &tag, 2);
        enqueue(std::move(msg));
    }

    // Legacy API (used by mrd_tcp_listener)
    void post_header(const std::string& xml)    { send_header(xml); }
    void post_config(const std::string& config) { send_config_file(config); }
    void post_config_text(const std::string& t) { send_config_text(t); }
    void post_frame(uint16_t tag, const std::string& body) {
        send_typed_frame(tag, body.data(), body.size());
    }
    void post_close() { send_close(); }

    void stop() {
        running_.store(false);
        cv_.notify_all();
        if (writer_.joinable()) writer_.join();
        // Cancel blocking read by closing socket
        {
            std::lock_guard<std::mutex> lk(connect_mtx_);
            if (socket_ && socket_->is_open()) {
                boost::system::error_code ec;
                socket_->cancel(ec);
                socket_->shutdown(tcp::socket::shutdown_both, ec);
                socket_->close(ec);
            }
        }
        if (reader_.joinable()) reader_.join();
        connected_.store(false);
    }

    bool is_connected() const { return connected_.load(); }

private:
    std::string recon_host_;
    uint16_t recon_port_;
    ImageCallback on_image_;
    FailureCallback on_failure_;

    std::atomic<bool> running_{true};
    std::atomic<bool> connected_{false};

    net::io_context ioc_;
    std::unique_ptr<tcp::socket> socket_;
    // connect_mtx_ protects socket creation/destruction only, NOT read/write.
    // TCP is full-duplex: one thread reads, another writes, no mutex needed.
    std::mutex connect_mtx_;

    std::queue<std::vector<uint8_t>> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;

    std::thread writer_;
    std::thread reader_;

    void enqueue(std::vector<uint8_t> msg) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (queue_.size() > 10000) {
            LOG_WARN("Recon queue full (>10000), dropping");
            return;
        }
        queue_.push(std::move(msg));
        cv_.notify_one();
    }

    bool try_connect() {
        std::lock_guard<std::mutex> lk(connect_mtx_);
        try {
            if (socket_ && socket_->is_open()) {
                boost::system::error_code ec;
                socket_->shutdown(tcp::socket::shutdown_both, ec);
                socket_->close(ec);
            }
            socket_.reset();
            connected_.store(false);

            tcp::resolver resolver(ioc_);
            auto endpoints = resolver.resolve(recon_host_, std::to_string(recon_port_));
            auto sock = std::make_unique<tcp::socket>(ioc_);
            net::connect(*sock, endpoints);
            sock->set_option(tcp::no_delay(true));
            socket_ = std::move(sock);
            connected_.store(true);
            LOG_INFO("Connected to recon at " << recon_host_ << ":" << recon_port_);

            // Start reader thread for this connection
            if (reader_.joinable()) reader_.join();
            reader_ = std::thread(&ReconForwarder::read_loop, this);
            return true;
        } catch (const std::exception& e) {
            LOG_WARN("Failed to connect to recon: " << e.what());
            connected_.store(false);
            return false;
        }
    }

    // Writer calls this — no mutex on socket (full-duplex safe)
    bool write_bytes(const void* data, size_t len) {
        if (!socket_ || !socket_->is_open()) return false;
        try {
            net::write(*socket_, net::buffer(data, len));
            return true;
        } catch (...) {
            connected_.store(false);
            return false;
        }
    }

    void write_loop() {
        while (running_.load()) {
            std::vector<uint8_t> msg;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait_for(lk, std::chrono::milliseconds(100),
                             [this] { return !queue_.empty() || !running_.load(); });
                if (queue_.empty()) continue;
                msg = std::move(queue_.front());
                queue_.pop();
            }

            // Connect once, keep the connection for the entire scan.
            // Only reconnect if not connected at all (first message or after
            // a previous connection was lost). Do NOT reconnect on write
            // failure — that would split a scan across multiple TCP sessions
            // and mock_recon would get CONFIG on one connection, METADATA on
            // another, etc.
            if (!connected_.load()) {
                if (!try_connect()) {
                    LOG_WARN("Recon not available, dropping message");
                    if (on_failure_) { try { on_failure_(); } catch (...) {} }
                    continue;
                }
            }

            if (!write_bytes(msg.data(), msg.size())) {
                LOG_WARN("Write to recon failed, dropping message");
                connected_.store(false);
                if (on_failure_) { try { on_failure_(); } catch (...) {} }
                // Do NOT reconnect here. The next message will see
                // connected_==false and reconnect then, starting a
                // fresh TCP session. This ensures CONFIG is always the
                // first message on a new connection.
            }
        }

        // Drain remaining messages on current connection
        std::lock_guard<std::mutex> lk(mtx_);
        while (!queue_.empty()) {
            auto& msg = queue_.front();
            if (connected_.load()) write_bytes(msg.data(), msg.size());
            queue_.pop();
        }
    }

    // Reader calls this — no mutex on socket (full-duplex safe)
    bool read_bytes(void* buf, size_t n) {
        if (!socket_ || !socket_->is_open()) return false;
        try {
            boost::system::error_code ec;
            net::read(*socket_, net::buffer(buf, n), ec);
            if (ec) { connected_.store(false); return false; }
            return true;
        } catch (...) {
            connected_.store(false);
            return false;
        }
    }

    void read_loop() {
        LOG_INFO("Recon reader started");
        while (running_.load() && connected_.load()) {
            uint16_t msg_id = 0;
            if (!read_bytes(&msg_id, 2)) break;

            if (msg_id == MRD_MESSAGE_ISMRMRD_IMAGE) {
                read_image_from_recon();
            } else if (msg_id == MRD_MESSAGE_CLOSE) {
                LOG_INFO("Recon sent CLOSE");
                connected_.store(false);
                break;
            } else if (msg_id == MRD_MESSAGE_TEXT) {
                uint32_t len = 0;
                if (!read_bytes(&len, 4)) break;
                std::vector<uint8_t> buf(len);
                if (!read_bytes(buf.data(), len)) break;
                LOG_INFO("Recon TEXT: " << std::string(buf.begin(), buf.end()));
            } else {
                LOG_WARN("Unexpected MRD message from recon: " << msg_id);
                connected_.store(false);
                break;
            }
        }
        LOG_INFO("Recon reader ended");
    }

    void read_image_from_recon() {
        std::vector<uint8_t> hdr(IMAGE_HEADER_BYTES);
        if (!read_bytes(hdr.data(), IMAGE_HEADER_BYTES)) return;
        auto* ihdr = reinterpret_cast<const ISMRMRD::ImageHeader*>(hdr.data());

        uint64_t attr_len = 0;
        if (!read_bytes(&attr_len, 8)) return;

        std::vector<uint8_t> attr(attr_len);
        if (attr_len > 0 && !read_bytes(attr.data(), attr_len)) return;

        size_t pixel_bytes = static_cast<size_t>(ihdr->matrix_size[0])
                           * ihdr->matrix_size[1]
                           * std::max<uint16_t>(ihdr->matrix_size[2], 1)
                           * std::max<uint16_t>(ihdr->channels, 1)
                           * ISMRMRD::ismrmrd_sizeof_data_type(ihdr->data_type);
        std::vector<uint8_t> pixels(pixel_bytes);
        if (!read_bytes(pixels.data(), pixel_bytes)) return;

        LOG_INFO("Received image from recon: "
                 << ihdr->matrix_size[0] << "x" << ihdr->matrix_size[1]);

        if (on_image_) {
            size_t total = IMAGE_HEADER_BYTES + 8 + attr_len + pixel_bytes;
            std::vector<uint8_t> wire(total);
            size_t off = 0;
            std::memcpy(wire.data() + off, hdr.data(), IMAGE_HEADER_BYTES); off += IMAGE_HEADER_BYTES;
            std::memcpy(wire.data() + off, &attr_len, 8); off += 8;
            if (attr_len > 0) { std::memcpy(wire.data() + off, attr.data(), attr_len); off += attr_len; }
            std::memcpy(wire.data() + off, pixels.data(), pixel_bytes);
            try { on_image_(wire.data(), wire.size()); } catch (...) {}
        }
    }
};

} // namespace mrd
