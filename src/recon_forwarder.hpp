/*
 * ReconForwarder — forwards scanner MRD TCP messages to a reconstruction service.
 *
 * Design: simple TCP pipe, matching python-ismrmrd-server's connection model.
 * One socket per scan. Sequential writes. A reader thread reads images back.
 * No reconnect mid-scan. No queues. No complexity.
 *
 * Lifecycle:
 *   1. Scanner sends CONFIG → marshal calls begin_session() → connects to recon
 *   2. Marshal forwards each message via send() → writes to the socket
 *   3. Reader thread reads IMAGE(1022) responses → calls on_image callback
 *   4. Scanner sends CLOSE → marshal calls end_session() → sends CLOSE, waits for reader
 *   5. Next scan: new begin_session() → new connection
 */

#pragma once

#include <boost/asio.hpp>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mrd_stream_tags.hpp"
#include "logging.hpp"

namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace mrd {

class ReconForwarder {
public:
    using ImageCallback = std::function<void(const void*, size_t)>;
    using FailureCallback = std::function<void()>;

    ReconForwarder(const std::string& host, uint16_t port,
                   ImageCallback on_image, FailureCallback on_failure)
        : recon_host_(host), recon_port_(port),
          on_image_(std::move(on_image)), on_failure_(std::move(on_failure)) {}

    ~ReconForwarder() { end_session(); }

    // --- Session lifecycle (called from marshal's MRD TCP listener thread) ---

    // Connect to recon, start reader thread. Call ONCE per scan, before any send().
    bool begin_session() {
        end_session();  // clean up any previous session

        try {
            tcp::resolver resolver(ioc_);
            auto endpoints = resolver.resolve(recon_host_, std::to_string(recon_port_));
            socket_ = std::make_unique<tcp::socket>(ioc_);
            net::connect(*socket_, endpoints);
            socket_->set_option(tcp::no_delay(true));
            connected_.store(true);
            LOG_INFO("Connected to recon at " << recon_host_ << ":" << recon_port_);

            // Start reader thread for IMAGE responses
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

    // Called by marshal to stop everything
    void stop() { end_session(); }

private:
    std::string recon_host_;
    uint16_t recon_port_;
    ImageCallback on_image_;
    FailureCallback on_failure_;

    net::io_context ioc_;
    std::unique_ptr<tcp::socket> socket_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> reader_running_{false};
    std::thread reader_;

    void write_all(const void* data, size_t len) {
        if (!connected_.load() || !socket_ || !socket_->is_open()) {
            LOG_WARN("Not connected to recon, dropping message");
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

    // Reader thread: reads IMAGE(1022) and CLOSE(4) from recon
    void read_loop() {
        LOG_INFO("Recon reader started");
        try {
            while (reader_running_.load() && connected_.load()) {
                uint16_t msg_id = 0;
                if (!read_exact(&msg_id, 2)) break;

                if (msg_id == MRD_MESSAGE_ISMRMRD_IMAGE) {
                    read_image_from_recon();
                } else if (msg_id == MRD_MESSAGE_CLOSE) {
                    LOG_INFO("Recon sent CLOSE (batch done)");
                } else if (msg_id == MRD_MESSAGE_TEXT) {
                    // Read and log text message
                    uint32_t len = 0;
                    if (!read_exact(&len, 4)) break;
                    std::vector<uint8_t> buf(len);
                    if (!read_exact(buf.data(), len)) break;
                    LOG_INFO("Recon TEXT: " << std::string(buf.begin(), buf.end()));
                } else {
                    LOG_WARN("Unexpected MRD message from recon: " << msg_id);
                    break;
                }
            }
        } catch (const std::exception& e) {
            LOG_WARN("Recon reader error: " << e.what());
        }
        LOG_INFO("Recon reader ended");
    }

    bool read_exact(void* buf, size_t n) {
        if (!socket_ || !socket_->is_open()) return false;
        boost::system::error_code ec;
        net::read(*socket_, net::buffer(buf, n), ec);
        return !ec;
    }

    void read_image_from_recon() {
        // Read ImageHeader (198 bytes)
        std::vector<uint8_t> hdr_buf(IMAGE_HEADER_BYTES);
        if (!read_exact(hdr_buf.data(), IMAGE_HEADER_BYTES)) return;

        const auto* ihdr = reinterpret_cast<const ISMRMRD::ImageHeader*>(hdr_buf.data());

        // Read attribute length (8 bytes, uint64 LE)
        uint64_t attr_len = 0;
        if (!read_exact(&attr_len, 8)) return;

        // Read attribute string
        std::vector<uint8_t> attr(attr_len);
        if (attr_len > 0 && !read_exact(attr.data(), attr_len)) return;

        // Read pixel data
        size_t npixels = static_cast<size_t>(ihdr->matrix_size[0])
                       * ihdr->matrix_size[1]
                       * std::max<uint16_t>(ihdr->matrix_size[2], 1)
                       * ihdr->channels;
        size_t itemsize = 4; // default float
        switch (ihdr->data_type) {
            case ISMRMRD::ISMRMRD_USHORT: itemsize = 2; break;
            case ISMRMRD::ISMRMRD_SHORT:  itemsize = 2; break;
            case ISMRMRD::ISMRMRD_UINT:   itemsize = 4; break;
            case ISMRMRD::ISMRMRD_INT:    itemsize = 4; break;
            case ISMRMRD::ISMRMRD_FLOAT:  itemsize = 4; break;
            case ISMRMRD::ISMRMRD_DOUBLE: itemsize = 8; break;
            case ISMRMRD::ISMRMRD_CXFLOAT:  itemsize = 8; break;
            case ISMRMRD::ISMRMRD_CXDOUBLE: itemsize = 16; break;
            default: break;
        }
        size_t pixel_bytes = npixels * itemsize;
        std::vector<uint8_t> pixels(pixel_bytes);
        if (!read_exact(pixels.data(), pixel_bytes)) return;

        LOG_INFO("Received image from recon: "
                 << ihdr->matrix_size[0] << "x" << ihdr->matrix_size[1]);

        // Assemble full wire-format body for the callback
        size_t total = IMAGE_HEADER_BYTES + 8 + attr_len + pixel_bytes;
        std::vector<uint8_t> body(total);
        size_t off = 0;
        std::memcpy(body.data() + off, hdr_buf.data(), IMAGE_HEADER_BYTES); off += IMAGE_HEADER_BYTES;
        std::memcpy(body.data() + off, &attr_len, 8); off += 8;
        if (attr_len > 0) { std::memcpy(body.data() + off, attr.data(), attr_len); off += attr_len; }
        std::memcpy(body.data() + off, pixels.data(), pixel_bytes);

        if (on_image_) {
            try { on_image_(body.data(), body.size()); } catch (...) {}
        }
    }
};

} // namespace mrd
