/*
 * File: clients/kspace_streamer/kspace_streamer_main.cpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Full C ISMRMRD scanner mock over MRD TCP
 *
 * Protocol: raw TCP to marshal --mrd-port using python-ismrmrd-server's
 * 2-byte message ID framing (see connection.py).
 *
 * Wire sequence:
 *   CONFIG_FILE(1)    — 2B tag + 1024B null-padded config name
 *   METADATA_XML(3)   — 2B tag + 4B length + XML bytes
 *   ACQUISITION(1008) — 2B tag + 340B header + traj + samples  (× N)
 *   WAVEFORM(1026)    — 2B tag + 40B header + uint32 samples   (optional, ECG)
 *   CLOSE(4)          — 2B tag only
 *
 * Also runs a reader thread to receive IMAGE(1022) frames pushed back
 * by the marshal (reconstructed images returned on the same socket).
 *
 * Multi-slice, noise scans, ACQ_FIRST/LAST_IN_SLICE flags.
 * No HTTP. No X-MRD-* headers. No /v1/* paths.
 */

#include <boost/asio.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include <stdexcept>

#include <ismrmrd/ismrmrd.h>
#include <ismrmrd/waveform.h>
#include "mrd_stream_tags.hpp"
#include "phantom.hpp"

namespace net = boost::asio;
using tcp = net::ip::tcp;

struct Options {
    std::string host{"localhost"};
    std::string port{"9100"};         // marshal --mrd-port
    std::string config_name{"simplefft"};
    std::size_t volumes{0};           // 0 = infinite
    double interval{0.5};             // seconds between volumes
    std::uint16_t samples{128};
    std::uint16_t channels{1};
    std::uint16_t lines{128};
    std::uint16_t slices{1};
    std::size_t log_stride{1};
    bool send_ecg{false};             // send synthetic ECG waveforms
};

Options parse_args(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 < argc) return argv[++i];
            throw std::runtime_error("missing value for " + arg);
        };
        if (arg == "--host")           opt.host = next();
        else if (arg == "--port")      opt.port = next();
        else if (arg == "--config")    opt.config_name = next();
        else if (arg == "--volumes")   opt.volumes = std::stoull(next());
        else if (arg == "--interval")  opt.interval = std::stod(next());
        else if (arg == "--samples")   opt.samples = static_cast<uint16_t>(std::stoi(next()));
        else if (arg == "--channels")  opt.channels = static_cast<uint16_t>(std::stoi(next()));
        else if (arg == "--lines")     opt.lines = static_cast<uint16_t>(std::stoi(next()));
        else if (arg == "--slices")    opt.slices = static_cast<uint16_t>(std::stoi(next()));
        else if (arg == "--log-stride") opt.log_stride = std::stoull(next());
        else if (arg == "--ecg")       opt.send_ecg = true;
    }
    return opt;
}

// Generate minimal ISMRMRD XML header
std::string make_xml_header(uint16_t nx, uint16_t ny, uint16_t nz, uint16_t ncoils) {
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\"?>\n"
        << "<ismrmrdHeader xmlns=\"http://www.ismrmrd.org/ISMRMRD\">\n"
        << "  <experimentalConditions>\n"
        << "    <H1resonanceFrequency_Hz>123000000</H1resonanceFrequency_Hz>\n"
        << "  </experimentalConditions>\n"
        << "  <encoding>\n"
        << "    <encodedSpace>\n"
        << "      <matrixSize><x>" << nx << "</x><y>" << ny << "</y><z>" << nz << "</z></matrixSize>\n"
        << "      <fieldOfView_mm><x>256</x><y>256</y><z>5</z></fieldOfView_mm>\n"
        << "    </encodedSpace>\n"
        << "    <reconSpace>\n"
        << "      <matrixSize><x>" << nx << "</x><y>" << ny << "</y><z>" << nz << "</z></matrixSize>\n"
        << "      <fieldOfView_mm><x>256</x><y>256</y><z>5</z></fieldOfView_mm>\n"
        << "    </reconSpace>\n"
        << "    <encodingLimits>\n"
        << "      <kspace_encoding_step_1><minimum>0</minimum><maximum>" << (ny - 1)
        << "</maximum><center>" << (ny / 2) << "</center></kspace_encoding_step_1>\n"
        << "      <slice><minimum>0</minimum><maximum>" << (nz - 1)
        << "</maximum><center>" << (nz / 2) << "</center></slice>\n"
        << "    </encodingLimits>\n"
        << "    <trajectory>cartesian</trajectory>\n"
        << "  </encoding>\n"
        << "</ismrmrdHeader>\n";
    return oss.str();
}

// ── MRD TCP helpers ────────────────────────────────────────────

// Write all bytes to socket, blocking
void write_all(tcp::socket& sock, const void* data, size_t len) {
    net::write(sock, net::buffer(data, len));
}

// Send CONFIG_FILE (tag 1): 2B tag + 1024B null-padded name
void send_config_file(tcp::socket& sock, const std::string& name) {
    uint16_t tag = mrd::MRD_MESSAGE_CONFIG_FILE;
    uint8_t buf[2 + 1024] = {};
    std::memcpy(buf, &tag, 2);
    std::memcpy(buf + 2, name.data(), std::min(name.size(), size_t(1024)));
    write_all(sock, buf, sizeof(buf));
}

// Send METADATA_XML (tag 3): 2B tag + 4B length + XML\0
void send_metadata_xml(tcp::socket& sock, const std::string& xml) {
    std::string with_nul = xml + '\0';
    uint16_t tag = mrd::MRD_MESSAGE_METADATA_XML_TEXT;
    uint32_t len = static_cast<uint32_t>(with_nul.size());
    write_all(sock, &tag, 2);
    write_all(sock, &len, 4);
    write_all(sock, with_nul.data(), len);
}

// Send ACQUISITION (tag 1008): 2B tag + header + traj + samples
void send_acquisition(tcp::socket& sock, const ISMRMRD::AcquisitionHeader& hdr,
                      const std::complex<float>* samples, size_t sample_count) {
    uint16_t tag = mrd::MRD_MESSAGE_ISMRMRD_ACQUISITION;
    write_all(sock, &tag, 2);
    write_all(sock, &hdr, sizeof(hdr));
    // trajectory_dimensions == 0 → no trajectory bytes
    if (hdr.trajectory_dimensions > 0) {
        size_t traj_bytes = hdr.trajectory_dimensions * hdr.number_of_samples * sizeof(float);
        std::vector<float> traj(hdr.trajectory_dimensions * hdr.number_of_samples, 0.0f);
        write_all(sock, traj.data(), traj_bytes);
    }
    write_all(sock, samples, sample_count * sizeof(std::complex<float>));
}

// Send WAVEFORM (tag 1026): 2B tag + 40B header + uint32 samples
void send_waveform(tcp::socket& sock, uint16_t waveform_id,
                   const uint32_t* data, uint16_t num_samples, uint16_t num_channels,
                   float sample_time_us, uint32_t time_stamp) {
    ISMRMRD::ISMRMRD_WaveformHeader whdr;
    std::memset(&whdr, 0, sizeof(whdr));
    whdr.version = 1;
    whdr.number_of_samples = num_samples;
    whdr.channels = num_channels;
    whdr.sample_time_us = sample_time_us;
    whdr.waveform_id = waveform_id;
    whdr.time_stamp = time_stamp;

    uint16_t tag = mrd::MRD_MESSAGE_ISMRMRD_WAVEFORM;
    write_all(sock, &tag, 2);
    write_all(sock, &whdr, sizeof(whdr));
    write_all(sock, data, num_samples * num_channels * sizeof(uint32_t));
}

// Send CLOSE (tag 4): 2B tag only
void send_close(tcp::socket& sock) {
    uint16_t tag = mrd::MRD_MESSAGE_CLOSE;
    write_all(sock, &tag, 2);
}

// ── Reader thread for pushed-back images ───────────────────────

void reader_thread(tcp::socket& sock, std::atomic<bool>& running,
                   std::atomic<size_t>& images_received) {
    try {
        while (running.load()) {
            uint16_t tag = 0;
            boost::system::error_code ec;
            net::read(sock, net::buffer(&tag, 2), ec);
            if (ec) break;

            if (tag == mrd::MRD_MESSAGE_ISMRMRD_IMAGE) {
                // Read ImageHeader (198 bytes)
                std::vector<uint8_t> ihdr(mrd::IMAGE_HEADER_BYTES);
                net::read(sock, net::buffer(ihdr.data(), ihdr.size()), ec);
                if (ec) break;

                // Read attribute length (8 bytes, uint64 LE)
                uint64_t attr_len = 0;
                net::read(sock, net::buffer(&attr_len, 8), ec);
                if (ec) break;

                // Read attribute string
                std::vector<char> attr;
                if (attr_len > 0) {
                    attr.resize(attr_len);
                    net::read(sock, net::buffer(attr.data(), attr_len), ec);
                    if (ec) break;
                }

                // Read pixel data: matrix_size[0]*[1]*[2]*channels*itemsize
                const auto* imhdr = reinterpret_cast<const ISMRMRD::ImageHeader*>(ihdr.data());
                size_t npixels = static_cast<size_t>(imhdr->matrix_size[0]) *
                                 imhdr->matrix_size[1] *
                                 std::max(uint16_t(1), imhdr->matrix_size[2]) *
                                 imhdr->channels;
                size_t itemsize = 4; // default float
                switch (imhdr->data_type) {
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
                net::read(sock, net::buffer(pixels.data(), pixel_bytes), ec);
                if (ec) break;

                images_received.fetch_add(1);
                size_t n = images_received.load();
                if (n == 1 || n % 10 == 0)
                    std::cout << "kspace_streamer: received " << n << " reconstructed image(s) back\n";
                if (!attr.empty()) {
                    std::string attr_text(attr.begin(), attr.end());
                    if (attr_text.find("ReconFailure") != std::string::npos) {
                        std::cout << "kspace_streamer: received recon failure image\n";
                    }
                }
            }
            else if (tag == mrd::MRD_MESSAGE_CLOSE) {
                std::cout << "kspace_streamer: received CLOSE from marshal\n";
                break;
            }
            else if (tag == mrd::MRD_MESSAGE_TEXT ||
                     tag == mrd::MRD_MESSAGE_CONFIG_TEXT ||
                     tag == mrd::MRD_MESSAGE_METADATA_XML_TEXT) {
                uint32_t len = 0;
                net::read(sock, net::buffer(&len, 4), ec);
                if (ec) break;
                std::vector<char> payload(len);
                if (len > 0) net::read(sock, net::buffer(payload.data(), payload.size()), ec);
                if (ec) break;
            }
            else if (tag == mrd::MRD_MESSAGE_CONFIG_FILE) {
                std::vector<char> payload(1024);
                net::read(sock, net::buffer(payload.data(), payload.size()), ec);
                if (ec) break;
            }
            else if (tag == mrd::MRD_MESSAGE_ISMRMRD_WAVEFORM) {
                ISMRMRD::WaveformHeader whdr;
                net::read(sock, net::buffer(&whdr, mrd::WAVEFORM_HEADER_BYTES), ec);
                if (ec) break;
                size_t data_bytes = size_t(whdr.number_of_samples) * whdr.channels * sizeof(uint32_t);
                std::vector<uint8_t> payload(data_bytes);
                if (data_bytes > 0)
                    net::read(sock, net::buffer(payload.data(), payload.size()), ec);
                if (ec) break;
            }
            else {
                // Unknown tag on return path — skip by disconnecting
                std::cout << "kspace_streamer: unknown return tag " << tag << ", stopping reader\n";
                break;
            }
        }
    } catch (...) {}
    std::cout << "kspace_streamer: reader thread exited (total images: "
	              << images_received.load() << ")\n";
}

struct ReaderGuard {
    tcp::socket& sock;
    std::atomic<bool>& running;
    std::thread& reader;

    ~ReaderGuard() {
        running.store(false);
        boost::system::error_code ec;
        sock.shutdown(tcp::socket::shutdown_both, ec);
        if (reader.joinable()) reader.join();
    }
};

// ── Synthetic ECG generation ───────────────────────────────────

float generate_ecg_sample(double t, double baseline_hz) {
    float p_wave = 0.15f * static_cast<float>(std::sin(2 * M_PI * baseline_hz * t));
    double qrs_offset = std::fmod(t * baseline_hz, 1.0);
    float qrs_wave = 0.0f;
    if (qrs_offset > 0.15 && qrs_offset < 0.25)
        qrs_wave = 2.0f * static_cast<float>(std::sin(2 * M_PI * 10 * (qrs_offset - 0.15)));
    float t_wave = 0.3f * static_cast<float>(std::sin(2 * M_PI * baseline_hz * t - M_PI / 3.0));
    return p_wave + qrs_wave + t_wave;
}

// ── main ───────────────────────────────────────────────────────

int main(int argc, char** argv) {
    try {
        Options opt = parse_args(argc, argv);

        std::cout << std::unitbuf;
        std::cerr << std::unitbuf;

        std::cout << "kspace_streamer: MRD TCP scanner mock\n"
                  << "  marshal: " << opt.host << ":" << opt.port << "\n"
                  << "  config: " << opt.config_name << "\n"
                  << "  samples: " << opt.samples
                  << " channels: " << opt.channels
                  << " lines: " << opt.lines
                  << " slices: " << opt.slices
                  << " ecg: " << (opt.send_ecg ? "yes" : "no") << "\n";

        net::io_context ioc;
        tcp::resolver resolver{ioc};
        tcp::socket sock{ioc};

        // Connect to marshal MRD TCP port
        auto endpoints = resolver.resolve(opt.host, opt.port);
        net::connect(sock, endpoints);
        sock.set_option(tcp::no_delay(true));
        std::cout << "kspace_streamer: connected to " << opt.host << ":" << opt.port << "\n";

        std::atomic<bool> reader_running{true};
        std::atomic<size_t> images_received{0};

        // Start reader thread BEFORE sending data. It MUST run during the scan
        // to drain IMAGE(1022) pushed back by the marshal. Without it, TCP
        // buffers fill and the entire pipeline deadlocks.
        // Concurrent sync read+write on the same TCP fd is safe on Linux
        // (same pattern as python-ismrmrd-server client.py: separate process
        // for reading, main thread for writing, same socket).
        std::thread reader([&]() {
            reader_thread(sock, reader_running, images_received);
        });
        ReaderGuard reader_guard{sock, reader_running, reader};

        // Send CONFIG_FILE + METADATA_XML
        send_config_file(sock, opt.config_name);
        std::string xml = make_xml_header(opt.samples, opt.lines, opt.slices, opt.channels);
        send_metadata_xml(sock, xml);
        std::cout << "kspace_streamer: config+header sent\n";

        const size_t nx = opt.samples;
        const size_t ny = opt.lines;
        const size_t ncoils = opt.channels;

        // Scratch buffers
        std::vector<double> phantom_img;
        std::vector<std::complex<float>> slice_kspace(nx * ny);
        std::vector<std::complex<float>> line_data(nx * ncoils);
        std::mt19937 rng{std::random_device{}()};
        std::normal_distribution<float> gauss{0.0f, 0.05f};

        // ECG state
        double ecg_time = 0.0;
        const double ecg_rate_hz = 100.0;    // 100 Hz sampling
        const double heart_rate_hz = 72.0 / 60.0;  // 72 BPM
        const uint16_t ecg_samples_per_batch = 100;

        size_t volume_index = 0;
        const size_t total = opt.volumes;
        auto next_deadline = std::chrono::steady_clock::now();

        while (total == 0 || volume_index < total) {
            double rotation = 0.05 * static_cast<double>(volume_index);
            double brightness = 0.75 + 0.25 * std::sin(0.1 * static_cast<double>(volume_index));

            size_t acq_count = 0;
            for (uint16_t slice = 0; slice < opt.slices; ++slice) {
                double slice_rot = rotation + 0.1 * slice;
                double slice_wt = std::cos(0.5 * M_PI *
                    (static_cast<double>(slice) - (opt.slices - 1) / 2.0) /
                    std::max(1.0, (opt.slices - 1) / 2.0));
                double slice_bright = brightness * std::max(0.25, slice_wt * slice_wt);

                kspace_sim::build_shepp_logan(nx, ny, slice_rot, slice_bright, phantom_img);
                kspace_sim::image_to_kspace(phantom_img, nx, ny, slice_kspace);

                for (uint16_t line = 0; line < ny; ++line) {
                    ISMRMRD::AcquisitionHeader ahdr;
                    std::memset(&ahdr, 0, sizeof(ahdr));
                    ahdr.version = 1;
                    ahdr.number_of_samples = opt.samples;
                    ahdr.active_channels = opt.channels;
                    ahdr.available_channels = opt.channels;
                    ahdr.trajectory_dimensions = 0;
                    ahdr.sample_time_us = 10.0f;
                    ahdr.scan_counter = static_cast<uint32_t>(
                        (volume_index * opt.slices + slice) * ny + line);
                    ahdr.idx.slice = slice;
                    ahdr.idx.kspace_encode_step_1 = line;
                    ahdr.idx.repetition = static_cast<uint16_t>(volume_index % 65535);

                    ahdr.flags = 0;
                    if (line == 0)
                        ahdr.flags |= (1ULL << (ISMRMRD::ISMRMRD_ACQ_FIRST_IN_SLICE - 1));
                    if (line == ny - 1)
                        ahdr.flags |= (1ULL << (ISMRMRD::ISMRMRD_ACQ_LAST_IN_SLICE - 1));

                    // Build per-channel k-space line with noise
                    for (size_t ch = 0; ch < ncoils; ++ch) {
                        for (size_t s = 0; s < nx; ++s) {
                            auto& src = slice_kspace[line * nx + s];
                            line_data[ch * nx + s] = std::complex<float>(
                                src.real() + gauss(rng), src.imag() + gauss(rng));
                        }
                    }

                    send_acquisition(sock, ahdr, line_data.data(),
                                     nx * ncoils);
                    ++acq_count;
                }

                // Send ECG waveform after each slice (if enabled)
                if (opt.send_ecg) {
                    std::vector<uint32_t> ecg_data(ecg_samples_per_batch);
                    for (uint16_t i = 0; i < ecg_samples_per_batch; ++i) {
                        float val = generate_ecg_sample(ecg_time, heart_rate_hz);
                        // Scale to uint32 range (center at 2^31)
                        ecg_data[i] = static_cast<uint32_t>(
                            static_cast<int64_t>(val * 1e6) + INT32_MAX);
                        ecg_time += 1.0 / ecg_rate_hz;
                    }
                    uint32_t ts = static_cast<uint32_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count() & 0xFFFFFFFF);
                    send_waveform(sock, 0 /* waveform_id=0 → ECG */,
                                  ecg_data.data(), ecg_samples_per_batch, 1,
                                  static_cast<float>(1e6 / ecg_rate_hz), ts);
                }
            }

            if (volume_index % opt.log_stride == 0)
                std::cout << "volume " << volume_index << ": " << acq_count
                          << " acquisitions sent\n";

            ++volume_index;

            if (opt.interval > 0.0) {
                next_deadline += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(opt.interval));
                auto now = std::chrono::steady_clock::now();
                if (next_deadline > now)
                    std::this_thread::sleep_until(next_deadline);
                else
                    next_deadline = now;
            }
        }

        // Send CLOSE
        send_close(sock);
        std::cout << "kspace_streamer: CLOSE sent, " << volume_index << " volumes\n";

        // Wait for reader to finish (marshal may push final images after CLOSE)
        std::this_thread::sleep_for(std::chrono::seconds(2));
        reader_running.store(false);

        // Shutdown write side to unblock reader
        boost::system::error_code ec;
        sock.shutdown(tcp::socket::shutdown_send, ec);
        if (reader.joinable()) reader.join();

        sock.close();
        std::cout << "kspace_streamer: done. Received " << images_received.load()
                  << " reconstructed images.\n";

    } catch (const std::exception& e) {
        std::cerr << "kspace_streamer error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
