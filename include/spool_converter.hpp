/*
 * File: include/spool_converter.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Read a raw MRD spool file (written by SpoolWriter) and
 *          replay each record into an ISMRMRD HDF5 file via MrdSink.
 *
 * Order-agnostic, like python-ismrmrd-server's reference server:
 * the sink opens lazily on first record; CONFIG_FILE / CONFIG_TEXT
 * / TEXT / METADATA_XML are written as they appear regardless of
 * arrival order. METADATA_XML sets the XML header via set_header().
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <ismrmrd/ismrmrd.h>
#include <ismrmrd/waveform.h>

#include "mrd_sink.hpp"
#include "mrd_stream_tags.hpp"

namespace mrd {

struct SpoolConvertStats {
    uint64_t records_read{0};
    uint32_t acq_written{0};
    uint32_t img_written{0};
    uint32_t wf_written{0};
    uint32_t text_written{0};
    bool truncated{false};
    std::string error;

    bool ok() const { return error.empty() && !truncated; }
};

// Scanner-side spool conversion: understands ACQ / IMAGE / WAVEFORM
// / CONFIG_FILE / CONFIG_TEXT / METADATA_XML / TEXT.
// Recon-side spool conversion: understands IMAGE / WAVEFORM / TEXT.
// Both use the same replay function; unknown tags for the given side
// are skipped (logged, not fatal).

inline bool parse_length_prefixed_text(const uint8_t* body, uint32_t len,
                                       std::string& out)
{
    // CONFIG_TEXT / METADATA_XML / TEXT carry [uint32 inner_len][text+NUL].
    if (len < sizeof(uint32_t)) return false;
    uint32_t inner = 0;
    std::memcpy(&inner, body, sizeof(inner));
    if (inner == 0) { out.clear(); return true; }
    if (len < sizeof(uint32_t) + inner) return false;
    const char* p = reinterpret_cast<const char*>(body + sizeof(uint32_t));
    // Strip trailing NUL if present.
    size_t n = inner;
    while (n > 0 && p[n - 1] == '\0') --n;
    out.assign(p, n);
    return true;
}

inline SpoolConvertStats convert_spool_to_hdf5(
    const std::filesystem::path& spool_path,
    const std::filesystem::path& h5_path,
    bool is_scanner_side)
{
    SpoolConvertStats stats;
    FILE* fp = std::fopen(spool_path.c_str(), "rb");
    if (!fp) {
        stats.error = std::string("open spool failed: ") + std::strerror(errno);
        return stats;
    }

    std::unique_ptr<MrdSink> sink;
    auto ensure_sink = [&] {
        if (!sink) sink = std::make_unique<MrdSink>(h5_path);
    };

    uint32_t text_count = 0;
    std::vector<uint8_t> body;

    while (true) {
        uint16_t tag = 0;
        uint32_t len = 0;
        size_t got = std::fread(&tag, sizeof(tag), 1, fp);
        if (got == 0) break; // clean EOF
        if (got != 1) { stats.truncated = true; break; }
        if (std::fread(&len, sizeof(len), 1, fp) != 1) { stats.truncated = true; break; }

        body.resize(len);
        if (len > 0) {
            if (std::fread(body.data(), 1, len, fp) != len) {
                stats.truncated = true;
                break;
            }
        }
        stats.records_read++;

        try {
            switch (tag) {
            case MRD_MESSAGE_CONFIG_FILE: {
                ensure_sink();
                // Body is a fixed 1024-byte C string; trim trailing NULs.
                size_t n = len;
                while (n > 0 && body[n - 1] == '\0') --n;
                sink->write_string_dataset("config_file",
                    std::string(reinterpret_cast<const char*>(body.data()), n));
                break;
            }
            case MRD_MESSAGE_CONFIG_TEXT: {
                ensure_sink();
                std::string s;
                if (parse_length_prefixed_text(body.data(), len, s))
                    sink->write_string_dataset("config", s);
                break;
            }
            case MRD_MESSAGE_METADATA_XML_TEXT: {
                ensure_sink();
                std::string xml;
                if (parse_length_prefixed_text(body.data(), len, xml))
                    sink->set_header(xml);
                break;
            }
            case MRD_MESSAGE_TEXT: {
                ensure_sink();
                std::string s;
                if (parse_length_prefixed_text(body.data(), len, s)) {
                    sink->write_string_dataset(
                        "text_" + std::to_string(text_count++), s);
                    stats.text_written++;
                }
                break;
            }
            case MRD_MESSAGE_ISMRMRD_ACQUISITION: {
                if (!is_scanner_side) break; // only scanner dumps acqs
                if (len < sizeof(ISMRMRD::AcquisitionHeader)) break;
                const auto* hdr =
                    reinterpret_cast<const ISMRMRD::AcquisitionHeader*>(body.data());
                const size_t hdr_bytes = sizeof(ISMRMRD::AcquisitionHeader);
                const size_t traj_bytes = size_t(hdr->trajectory_dimensions) *
                                          hdr->number_of_samples * sizeof(float);
                const size_t sample_bytes = size_t(hdr->number_of_samples) *
                                            hdr->active_channels *
                                            2 * sizeof(float);
                if (len < hdr_bytes + traj_bytes + sample_bytes) break;
                ensure_sink();
                ISMRMRD::Acquisition acq(hdr->number_of_samples,
                                         hdr->active_channels,
                                         hdr->trajectory_dimensions);
                acq.setHead(*hdr);
                if (traj_bytes > 0)
                    std::memcpy(acq.getTrajPtr(),
                                body.data() + hdr_bytes, traj_bytes);
                if (sample_bytes > 0)
                    std::memcpy(acq.getDataPtr(),
                                body.data() + hdr_bytes + traj_bytes,
                                sample_bytes);
                sink->append_acquisition(acq);
                stats.acq_written++;
                break;
            }
            case MRD_MESSAGE_ISMRMRD_IMAGE: {
                // Wire body: [ImageHeader][uint64 attr_len][attr][pixels]
                if (len < IMAGE_HEADER_BYTES + sizeof(uint64_t)) break;
                const auto* hdr =
                    reinterpret_cast<const ISMRMRD::ImageHeader*>(body.data());
                uint64_t attr_len = 0;
                std::memcpy(&attr_len, body.data() + IMAGE_HEADER_BYTES,
                            sizeof(attr_len));
                const size_t attr_off = IMAGE_HEADER_BYTES + sizeof(uint64_t);
                if (len < attr_off) break;
                if (attr_len > len - attr_off) break;
                const size_t pixel_off = attr_off + static_cast<size_t>(attr_len);
                ensure_sink();
                const std::string varname =
                    "image_" + std::to_string(hdr->image_series_index);
                sink->append_image(
                    varname, *hdr,
                    reinterpret_cast<const char*>(body.data() + attr_off),
                    static_cast<size_t>(attr_len),
                    body.data() + pixel_off, len - pixel_off);
                stats.img_written++;
                break;
            }
            case MRD_MESSAGE_ISMRMRD_WAVEFORM: {
                if (len < WAVEFORM_HEADER_BYTES) break;
                const auto* hdr =
                    reinterpret_cast<const ISMRMRD::WaveformHeader*>(body.data());
                const size_t data_bytes =
                    size_t(hdr->number_of_samples) * hdr->channels *
                    sizeof(uint32_t);
                if (len < WAVEFORM_HEADER_BYTES + data_bytes) break;
                ensure_sink();
                ISMRMRD::Waveform wf(hdr->number_of_samples, hdr->channels);
                std::memcpy(&wf.head, body.data(), WAVEFORM_HEADER_BYTES);
                if (data_bytes > 0)
                    std::memcpy(wf.data, body.data() + WAVEFORM_HEADER_BYTES,
                                data_bytes);
                sink->append_waveform(wf);
                stats.wf_written++;
                break;
            }
            case MRD_MESSAGE_CLOSE:
                // CLOSE is a wire-level terminator; nothing to archive.
                break;
            default:
                // Unknown tag. Skipped intentionally; not fatal.
                break;
            }
        } catch (const std::exception& e) {
            stats.error = std::string("replay failed at record ") +
                std::to_string(stats.records_read) + ": " + e.what();
            break;
        }
    }

    std::fclose(fp);
    // Destroying sink flushes + closes the HDF5 file.
    sink.reset();
    return stats;
}

} // namespace mrd
