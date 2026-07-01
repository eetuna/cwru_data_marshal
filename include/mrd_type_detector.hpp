/*
 * File: include/mrd_type_detector.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Detect ISMRMRD message type from raw POST /frame body
 *
 * Uses struct sizes and heuristic header checks. Never returns 400 —
 * unknown data is classified as UNKNOWN and forwarded anyway.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <ismrmrd/ismrmrd.h>
#include <ismrmrd/waveform.h>

#include "mrd_stream_tags.hpp"

namespace mrd {

enum class MrdDataType {
    ACQUISITION,
    IMAGE,
    WAVEFORM,
    UNKNOWN
};

inline const char* mrd_data_type_to_string(MrdDataType type)
{
    switch (type) {
    case MrdDataType::ACQUISITION: return "ACQUISITION";
    case MrdDataType::IMAGE:       return "IMAGE";
    case MrdDataType::WAVEFORM:    return "WAVEFORM";
    case MrdDataType::UNKNOWN:
    default:                       return "UNKNOWN";
    }
}

// Check if body looks like an ISMRMRD AcquisitionHeader
inline bool is_acquisition_header(const void* data, size_t size)
{
    if (size < ACQUISITION_HEADER_BYTES) return false;

    const auto* hdr = static_cast<const ISMRMRD::AcquisitionHeader*>(data);

    if (hdr->version == 0 || hdr->version > 10) return false;
    if (hdr->number_of_samples == 0 || hdr->number_of_samples > 65535) return false;
    if (hdr->active_channels == 0 || hdr->active_channels > 256) return false;
    if (hdr->trajectory_dimensions > 3) return false;
    if (hdr->available_channels < hdr->active_channels) return false;

    // Verify expected body size: header + trajectory + samples
    size_t expected = ACQUISITION_HEADER_BYTES
        + static_cast<size_t>(hdr->trajectory_dimensions) * hdr->number_of_samples * sizeof(float)
        + static_cast<size_t>(hdr->number_of_samples) * hdr->active_channels * sizeof(complex_float_t);
    if (size < expected) return false;

    return true;
}

// Check if body looks like an ISMRMRD ImageHeader
inline bool is_image_header(const void* data, size_t size)
{
    if (size < IMAGE_HEADER_BYTES) return false;

    const auto* hdr = static_cast<const ISMRMRD::ImageHeader*>(data);

    if (hdr->version == 0 || hdr->version > 10) return false;
    if (hdr->matrix_size[0] == 0 || hdr->matrix_size[0] > 4096) return false;
    if (hdr->matrix_size[1] == 0 || hdr->matrix_size[1] > 4096) return false;
    if (hdr->matrix_size[2] > 4096) return false;
    if (hdr->channels == 0 || hdr->channels > 256) return false;
    if (hdr->data_type == 0 || hdr->data_type > 10) return false;

    return true;
}

// Check if body looks like an ISMRMRD WaveformHeader
inline bool is_waveform_header(const void* data, size_t size)
{
    if (size < WAVEFORM_HEADER_BYTES) return false;

    const auto* hdr = static_cast<const ISMRMRD::WaveformHeader*>(data);

    if (hdr->version == 0 || hdr->version > 10) return false;
    if (hdr->number_of_samples == 0 || hdr->number_of_samples > 65535) return false;
    if (hdr->channels == 0 || hdr->channels > 256) return false;

    // Verify expected body size: header + samples
    size_t expected = WAVEFORM_HEADER_BYTES
        + static_cast<size_t>(hdr->number_of_samples) * hdr->channels * sizeof(uint32_t);
    if (size < expected) return false;

    return true;
}

// Detect MRD message type from raw body bytes.
// Order: ACQUISITION (most common in streaming), then IMAGE, then WAVEFORM, then UNKNOWN.
inline MrdDataType detect_mrd_type(const void* data, size_t size)
{
    if (data == nullptr || size < WAVEFORM_HEADER_BYTES)
        return MrdDataType::UNKNOWN;

    if (is_acquisition_header(data, size)) return MrdDataType::ACQUISITION;
    if (is_image_header(data, size))       return MrdDataType::IMAGE;
    if (is_waveform_header(data, size))    return MrdDataType::WAVEFORM;

    return MrdDataType::UNKNOWN;
}

} // namespace mrd
