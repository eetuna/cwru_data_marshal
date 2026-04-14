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
#include <cstdint>
#include <cstring>
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
#include "mrd_io.hpp"
#include "mrd_sink.hpp"
#include "mrd_stream_tags.hpp"
#include "mrd_type_detector.hpp"
#include "recon_forwarder.hpp"

namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace mrd {

inline void write_scanner_latest_image_h5(MarshalState& state, const uint8_t* body, size_t size)
{
    if (size < IMAGE_HEADER_BYTES + sizeof(uint64_t)) return;

    const auto* hdr = reinterpret_cast<const ISMRMRD::ImageHeader*>(body);
    uint64_t attr_len = 0;
    std::memcpy(&attr_len, body + IMAGE_HEADER_BYTES, sizeof(uint64_t));
    const size_t attr_off = IMAGE_HEADER_BYTES + sizeof(uint64_t);
    if (size < attr_off + attr_len) return;
    const size_t pixel_off = attr_off + static_cast<size_t>(attr_len);

    std::string xml;
    {
        std::lock_guard<std::mutex> lk(state.scan_mtx);
        xml = state.current_xml_header;
    }

    auto dest = scanner_dir(state.dump_dir) / "latest_image.h5";
    auto tmp = dest;
    tmp += ".tmp";

    std::error_code ec;
    std::filesystem::remove(tmp, ec);

    {
        MrdSink sink(tmp);
        if (!xml.empty()) sink.set_header(xml);
        sink.append_image("image_0", *hdr,
                          reinterpret_cast<const char*>(body + attr_off),
                          static_cast<size_t>(attr_len),
                          body + pixel_off,
                          size - pixel_off);
        sink.close();
    }

    std::filesystem::rename(tmp, dest, ec);
    if (ec) {
        std::filesystem::remove(dest, ec);
        ec.clear();
        std::filesystem::rename(tmp, dest, ec);
        if (ec) {
            LOG_WARN("Scanner latest H5 rename failed: " << ec.message());
            return;
        }
    }

    std::lock_guard<std::mutex> img_lk(state.latest_image_mtx);
    state.latest_image_path = dest.string();
    state.latest_image_error = false;
    state.latest_image_count++;
}

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

    // Push a recon MRD message back to the scanner over the existing scan socket.
    void push_message_to_scanner(uint16_t tag, const void* data, size_t len) {
        std::lock_guard<std::mutex> lk(scanner_mtx_);
        if (!scanner_socket_ || !scanner_socket_->is_open()) {
            LOG_WARN("No scanner connected, cannot push MRD message");
            return;
        }
        int fd = scanner_socket_->native_handle();
        if (fd < 0) {
            LOG_WARN("No scanner fd, cannot push MRD message");
            return;
        }
        if (write_exact_fd(fd, &tag, sizeof(tag)) &&
            (len == 0 || write_exact_fd(fd, data, len))) {
            LOG_DEBUG("Pushed MRD message to scanner tag=" << tag
                      << " (" << len << " bytes)");
        } else {
            LOG_WARN("Failed to push MRD message to scanner");
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
    std::shared_ptr<tcp::socket> scanner_socket_;
    std::atomic<bool> session_active_{false};

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

    void do_accept() {
        acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket sock) {
            if (!ec) {
                LOG_INFO("Scanner connected from " << sock.remote_endpoint());
                {
                    std::lock_guard<std::mutex> lk(scanner_mtx_);
                    scanner_socket_ = std::make_shared<tcp::socket>(std::move(sock));
                }
                auto socket_ptr = scanner_socket_;
                std::thread([this, socket_ptr]() {
                    handle_session(socket_ptr);
                }).detach();
            }
            do_accept();
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
                        state_.dump_recorder->set_scanner_config_file(config);
                    }
                    send_or_buffer_recon(MRD_MESSAGE_CONFIG_FILE, body);
                    break;
                }

                case MRD_MESSAGE_CONFIG_TEXT: {
                    uint32_t len = 0;
                    if (!read_exact(*sock, &len, 4)) goto done;
                    std::vector<uint8_t> body(4 + len);
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
                        state_.dump_recorder->set_scanner_config_text(config);
                    }
                    send_or_buffer_recon(MRD_MESSAGE_CONFIG_TEXT, body);
                    break;
                }

                case MRD_MESSAGE_METADATA_XML_TEXT: {
                    uint32_t len = 0;
                    if (!read_exact(*sock, &len, 4)) goto done;
                    std::vector<uint8_t> body(4 + len);
                    std::memcpy(body.data(), &len, 4);
                    if (len > 0 && !read_exact(*sock, body.data() + 4, len)) goto done;
                    const auto* payload = body.data() + 4;
                    std::string xml(reinterpret_cast<const char*>(payload),
                                    reinterpret_cast<const char*>(payload) + len);
                    auto nul = xml.find('\0');
                    if (nul != std::string::npos) xml.resize(nul);
                    LOG_INFO("METADATA_XML: " << xml.size() << " bytes");
                    // Parse <z> from encoding limits (slice count)
                    uint16_t nz = 1;
                    {
                        // Look for <slice><maximum>N</maximum> in encodingLimits
                        std::regex re_slice(R"(<slice>\s*<minimum>\d+</minimum>\s*<maximum>(\d+)</maximum>)");
                        std::smatch m;
                        if (std::regex_search(xml, m, re_slice)) {
                            nz = static_cast<uint16_t>(std::stoi(m[1].str()) + 1);
                        } else {
                            // Fallback: look for <z> in matrixSize
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
                        state_.expected_slices = nz;
                        state_.recon_failure_reported.store(false);
                        state_.latest_image_generation.fetch_add(1);
                        state_.slice_buffer.clear();
                    }
                    if (state_.dump_enabled && state_.dump_recorder) {
                        state_.dump_recorder->start_scan(scan_file, xml);
                    }
                    // Reset latest image pointer for new scan. The latest live-client
                    // file is a canonical ISMRMRD H5 written by handle_recon_image().
                    {
                        auto standalone = recon_dir(state_.dump_dir) / "latest_image.h5";
                        std::error_code ec;
                        std::filesystem::remove(standalone, ec);
                        std::lock_guard<std::mutex> img_lk(state_.latest_image_mtx);
                        state_.latest_image_path.clear();
                        state_.latest_image_error = false;
                        state_.latest_image_count = 0;
                    }
                    LOG_INFO("Expected slices (nz): " << nz);
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
                    state_.close_scan();
                    session_active_.store(false);
                    break;
                }

                case MRD_MESSAGE_TEXT: {
                    uint32_t len = 0;
                    if (!read_exact(*sock, &len, 4)) goto done;
                    std::vector<uint8_t> body(4 + len);
                    std::memcpy(body.data(), &len, 4);
                    if (len > 0 && !read_exact(*sock, body.data() + 4, len)) goto done;
                    std::string text(reinterpret_cast<const char*>(body.data() + 4), len);
                    auto nul = text.find('\0');
                    if (nul != std::string::npos) text.resize(nul);
                    LOG_INFO("TEXT: " << text);
                    if (state_.dump_enabled && state_.dump_recorder) {
                        state_.dump_recorder->append_scanner_text(text);
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
                        state_.dump_recorder->append_scanner_acquisition(
                            ahdr, std::vector<uint8_t>(traj), std::vector<uint8_t>(samples));
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
                    uint64_t attr_len = 0;
                    if (!read_exact(*sock, &attr_len, 8)) goto done;
                    std::vector<uint8_t> attr(attr_len);
                    if (attr_len > 0 && !read_exact(*sock, attr.data(), attr_len)) goto done;
                    size_t pixel_bytes = size_t(ihdr->matrix_size[0])
                                       * ihdr->matrix_size[1]
                                       * std::max<uint16_t>(ihdr->matrix_size[2], 1)
                                       * std::max<uint16_t>(ihdr->channels, 1)
                                       * ISMRMRD::ismrmrd_sizeof_data_type(ihdr->data_type);
                    std::vector<uint8_t> pixels(pixel_bytes);
                    if (!read_exact(*sock, pixels.data(), pixel_bytes)) goto done;

                    size_t total = IMAGE_HEADER_BYTES + 8 + attr_len + pixel_bytes;
                    std::vector<uint8_t> body(total);
                    size_t o = 0;
                    std::memcpy(body.data()+o, hdr_buf.data(), IMAGE_HEADER_BYTES); o += IMAGE_HEADER_BYTES;
                    std::memcpy(body.data()+o, &attr_len, 8); o += 8;
                    if (attr_len > 0) { std::memcpy(body.data()+o, attr.data(), attr_len); o += attr_len; }
                    std::memcpy(body.data()+o, pixels.data(), pixel_bytes);

                    LOG_DEBUG("IMAGE from scanner: "
                              << ihdr->matrix_size[0] << "x" << ihdr->matrix_size[1]);
                    if (state_.dump_enabled && state_.dump_recorder) {
                        state_.dump_recorder->append_scanner_image(
                            *ihdr, std::vector<uint8_t>(attr), std::vector<uint8_t>(pixels));
                    }
                    // Scanner-origin IMAGE is already reconstructed image data.
                    // Save/expose it for file-reading clients; do not send it to
                    // the k-space reconstruction service.
                    write_scanner_latest_image_h5(state_, body.data(), body.size());
                    break;
                }

                case MRD_MESSAGE_ISMRMRD_WAVEFORM: {
                    ISMRMRD::WaveformHeader whdr;
                    if (!read_exact(*sock, &whdr, WAVEFORM_HEADER_BYTES)) goto done;
                    size_t data_bytes = size_t(whdr.number_of_samples) * whdr.channels * 4;
                    std::vector<uint8_t> wf_data(data_bytes);
                    if (!read_exact(*sock, wf_data.data(), data_bytes)) goto done;

                    std::vector<uint8_t> body(WAVEFORM_HEADER_BYTES + data_bytes);
                    std::memcpy(body.data(), &whdr, WAVEFORM_HEADER_BYTES);
                    std::memcpy(body.data() + WAVEFORM_HEADER_BYTES, wf_data.data(), data_bytes);

                    if (state_.dump_enabled && state_.dump_recorder) {
                        state_.dump_recorder->append_scanner_waveform(
                            whdr, std::vector<uint8_t>(wf_data));
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
