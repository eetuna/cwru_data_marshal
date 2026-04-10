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
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

    // Push a reconstructed image back to the scanner (called from POST /image handler)
    void push_image_to_scanner(const void* data, size_t len) {
        std::lock_guard<std::mutex> lk(scanner_mtx_);
        if (!scanner_socket_ || !scanner_socket_->is_open()) {
            LOG_WARN("No scanner connected, cannot push image");
            return;
        }
        try {
            uint16_t tag = MRD_MESSAGE_ISMRMRD_IMAGE;
            net::write(*scanner_socket_, net::buffer(&tag, sizeof(tag)));
            net::write(*scanner_socket_, net::buffer(data, len));
            LOG_INFO("Pushed image to scanner (" << len << " bytes)");
        } catch (const std::exception& e) {
            LOG_WARN("Failed to push image to scanner: " << e.what());
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

        try {
            while (sock->is_open()) {
                uint16_t msg_id = 0;
                if (!read_exact(*sock, &msg_id, sizeof(msg_id))) break;

                switch (msg_id) {

                case MRD_MESSAGE_CONFIG_FILE: {
                    char buf[1024];
                    if (!read_exact(*sock, buf, 1024)) goto done;
                    std::string config(buf, strnlen(buf, 1024));
                    LOG_INFO("CONFIG_FILE: " << config);
                    std::lock_guard<std::mutex> lk(state_.scan_mtx);
                    state_.current_config = config;
                    state_.config_received.store(true);
                    if (forwarder_) forwarder_->post_config(config);
                    break;
                }

                case MRD_MESSAGE_CONFIG_TEXT: {
                    uint32_t len = 0;
                    if (!read_exact(*sock, &len, 4)) goto done;
                    std::vector<uint8_t> buf;
                    if (!read_exact(*sock, buf, len)) goto done;
                    std::string config(buf.begin(), buf.end());
                    auto nul = config.find('\0');
                    if (nul != std::string::npos) config.resize(nul);
                    LOG_INFO("CONFIG_TEXT: " << config);
                    std::lock_guard<std::mutex> lk(state_.scan_mtx);
                    state_.current_config = config;
                    state_.config_received.store(true);
                    if (forwarder_) forwarder_->post_config_text(config);
                    break;
                }

                case MRD_MESSAGE_METADATA_XML_TEXT: {
                    uint32_t len = 0;
                    if (!read_exact(*sock, &len, 4)) goto done;
                    std::vector<uint8_t> buf;
                    if (!read_exact(*sock, buf, len)) goto done;
                    std::string xml(buf.begin(), buf.end());
                    auto nul = xml.find('\0');
                    if (nul != std::string::npos) xml.resize(nul);
                    LOG_INFO("METADATA_XML: " << xml.size() << " bytes");
                    {
                        std::lock_guard<std::mutex> lk(state_.scan_mtx);
                        if (state_.scanner_sink) {
                            state_.scanner_sink->close();
                            state_.scanner_sink.reset();
                        }
                        auto path = scanner_dir(state_.dump_dir) / scan_filename();
                        state_.scanner_sink = std::make_unique<MrdSink>(path);
                        state_.scanner_sink->set_header(xml);
                        state_.current_xml_header = xml;
                    }
                    state_.header_received.store(true);
                    if (forwarder_) forwarder_->post_header(xml);
                    break;
                }

                case MRD_MESSAGE_CLOSE: {
                    LOG_INFO("CLOSE");
                    state_.close_scan();
                    if (forwarder_) forwarder_->post_close();
                    break;
                }

                case MRD_MESSAGE_TEXT: {
                    uint32_t len = 0;
                    if (!read_exact(*sock, &len, 4)) goto done;
                    std::vector<uint8_t> buf;
                    if (!read_exact(*sock, buf, len)) goto done;
                    LOG_INFO("TEXT: " << std::string(buf.begin(), buf.end()));
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

                    // Archive
                    {
                        std::lock_guard<std::mutex> lk(state_.scan_mtx);
                        if (state_.scanner_sink) {
                            ISMRMRD::Acquisition acq(ahdr.number_of_samples,
                                                     ahdr.active_channels,
                                                     ahdr.trajectory_dimensions);
                            acq.setHead(ahdr);
                            if (traj_bytes > 0)
                                std::memcpy(acq.getTrajPtr(), traj.data(), traj_bytes);
                            std::memcpy(acq.getDataPtr(), samples.data(), sample_bytes);
                            state_.scanner_sink->append_acquisition(acq);
                        }
                    }

                    // Forward raw bytes to recon with correct tag
                    if (forwarder_) {
                        std::string body(ACQUISITION_HEADER_BYTES + traj_bytes + sample_bytes, '\0');
                        std::memcpy(body.data(), &ahdr, ACQUISITION_HEADER_BYTES);
                        if (traj_bytes > 0)
                            std::memcpy(body.data() + ACQUISITION_HEADER_BYTES,
                                        traj.data(), traj_bytes);
                        std::memcpy(body.data() + ACQUISITION_HEADER_BYTES + traj_bytes,
                                    samples.data(), sample_bytes);
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

                    LOG_INFO("IMAGE from scanner: "
                             << ihdr->matrix_size[0] << "x" << ihdr->matrix_size[1]);
                    {
                        std::lock_guard<std::mutex> lk(state_.scan_mtx);
                        if (state_.scanner_sink) {
                            std::string var = "image_" + std::to_string(ihdr->image_series_index);
                            state_.scanner_sink->append_image(
                                var, *ihdr,
                                reinterpret_cast<const char*>(attr.data()), attr_len,
                                pixels.data(), pixel_bytes);
                        }
                    }
                    if (forwarder_) {
                        size_t total = IMAGE_HEADER_BYTES + 8 + attr_len + pixel_bytes;
                        std::string body(total, '\0');
                        size_t o = 0;
                        std::memcpy(body.data()+o, hdr_buf.data(), IMAGE_HEADER_BYTES); o += IMAGE_HEADER_BYTES;
                        std::memcpy(body.data()+o, &attr_len, 8); o += 8;
                        if (attr_len > 0) { std::memcpy(body.data()+o, attr.data(), attr_len); o += attr_len; }
                        std::memcpy(body.data()+o, pixels.data(), pixel_bytes);
                        forwarder_->post_frame(MRD_MESSAGE_ISMRMRD_IMAGE, body);
                    }
                    break;
                }

                case MRD_MESSAGE_ISMRMRD_WAVEFORM: {
                    ISMRMRD::WaveformHeader whdr;
                    if (!read_exact(*sock, &whdr, WAVEFORM_HEADER_BYTES)) goto done;
                    size_t data_bytes = size_t(whdr.number_of_samples) * whdr.channels * 4;
                    std::vector<uint8_t> wf_data(data_bytes);
                    if (!read_exact(*sock, wf_data.data(), data_bytes)) goto done;

                    {
                        std::lock_guard<std::mutex> lk(state_.scan_mtx);
                        if (state_.scanner_sink) {
                            ISMRMRD::Waveform wf(whdr.number_of_samples, whdr.channels);
                            std::memcpy(&wf.head, &whdr, WAVEFORM_HEADER_BYTES);
                            std::memcpy(wf.data, wf_data.data(), data_bytes);
                            state_.scanner_sink->append_waveform(wf);
                        }
                    }
                    if (forwarder_) {
                        std::string body(WAVEFORM_HEADER_BYTES + data_bytes, '\0');
                        std::memcpy(body.data(), &whdr, WAVEFORM_HEADER_BYTES);
                        std::memcpy(body.data() + WAVEFORM_HEADER_BYTES, wf_data.data(), data_bytes);
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
        LOG_INFO("MRD session ended");
        {
            std::lock_guard<std::mutex> lk(scanner_mtx_);
            scanner_socket_.reset();
        }
    }
};

} // namespace mrd
