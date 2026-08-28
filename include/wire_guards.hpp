/*
 * File: include/wire_guards.hpp
 * Purpose: Checked arithmetic + sanity caps for wire-parsed MRD sizes.
 *
 * Fixes HIGH #5, #6, #7 in MRI_MARSHAL_BUGS_FINAL_2026-04-18.md:
 *   - attr_len + aggregate body-size overflow (true OOB write on recon side,
 *     allocation-gated OOB write on scanner side)
 *   - pixel_bytes product overflow (4x uint16_t factors)
 *
 * All helpers are noexcept, return bool, and write the result via an out
 * pointer. Callers should abort parsing and goto done / return false on
 * failure rather than allocating.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace mrd {

// Hard caps. Nothing in a real ISMRMRD image stream should exceed these.
// They exist to reject malicious or buggy peer frames before allocation.

// Max attribute blob per image (ISMRMRD meta XML, typically KB).
inline constexpr std::size_t kMaxAttrBytes = 16ULL * 1024 * 1024;  // 16 MiB

// Max pixels per image (matrix_size[0] * matrix_size[1] * matrix_size[2] * channels).
// 4096 * 4096 * 512 * 16 = 128G pixels, well past any realistic MR image.
// This is just a sanity ceiling; dimension caps below kick in first.
inline constexpr std::size_t kMaxImagePixels = 1ULL << 30;  // ~1G pixels

// Per-dimension caps.
inline constexpr std::uint16_t kMaxImageDim = 8192;
inline constexpr std::uint16_t kMaxImageChannels = 256;

// ISMRMRD datatype sizes are at most 16 (complex double). Fence against a
// junk datatype reporting a huge size.
inline constexpr std::size_t kMaxDatatypeBytes = 16;

// Overall body-size cap for a single IMAGE frame. If a parsed attr_len +
// pixel_bytes + headers would exceed this, reject. 512 MiB leaves plenty
// of margin for huge 3D acquisitions; beyond that is a bug or attack.
inline constexpr std::size_t kMaxImageFrameBytes = 512ULL * 1024 * 1024;

// Per-frame cap on MRD length-prefix bodies (CONFIG_TEXT, METADATA_XML,
// TEXT). 64 MiB is far larger than any real XML header.
inline constexpr std::size_t kMaxLenPrefixBodyBytes = 64ULL * 1024 * 1024;

// checked_add: a + b -> out, returns false on overflow.
inline bool checked_add(std::size_t a, std::size_t b, std::size_t& out) noexcept {
    if (a > std::numeric_limits<std::size_t>::max() - b) return false;
    out = a + b;
    return true;
}

// checked_mul: a * b -> out, returns false on overflow.
inline bool checked_mul(std::size_t a, std::size_t b, std::size_t& out) noexcept {
    if (a == 0 || b == 0) { out = 0; return true; }
    if (a > std::numeric_limits<std::size_t>::max() / b) return false;
    out = a * b;
    return true;
}

// Validate + compute pixel_bytes for an IMAGE frame. Inputs are the raw
// uint16_t fields from ISMRMRD::ImageHeader. Returns false (and leaves out
// unspecified) if any cap is exceeded or the product overflows.
inline bool compute_pixel_bytes(std::uint16_t nx,
                                std::uint16_t ny,
                                std::uint16_t nz,
                                std::uint16_t channels,
                                std::size_t datatype_bytes,
                                std::size_t& out) noexcept {
    if (nx == 0 || nx > kMaxImageDim) return false;
    if (ny == 0 || ny > kMaxImageDim) return false;
    // nz and channels may legally be 0 in wire format; treat as 1 (matches
    // existing std::max<uint16_t>(..., 1) in both parsers).
    std::size_t z = nz == 0 ? 1 : nz;
    std::size_t c = channels == 0 ? 1 : channels;
    if (z > kMaxImageDim) return false;
    if (c > kMaxImageChannels) return false;
    if (datatype_bytes == 0 || datatype_bytes > kMaxDatatypeBytes) return false;

    std::size_t npixels = 0;
    if (!checked_mul(nx, ny, npixels)) return false;
    if (!checked_mul(npixels, z, npixels)) return false;
    if (!checked_mul(npixels, c, npixels)) return false;
    if (npixels > kMaxImagePixels) return false;
    if (!checked_mul(npixels, datatype_bytes, out)) return false;
    return true;
}

// Validate an attr_len parsed from wire against our cap.
inline bool validate_attr_len(std::uint64_t attr_len, std::size_t& out) noexcept {
    if (attr_len > kMaxAttrBytes) return false;
    out = static_cast<std::size_t>(attr_len);
    return true;
}

// Validate the aggregate body size for an IMAGE frame. image_header_bytes
// + 8 (attr_len prefix) + attr_len + pixel_bytes, with overflow + cap check.
inline bool compute_image_body_total(std::size_t image_header_bytes,
                                     std::size_t attr_len,
                                     std::size_t pixel_bytes,
                                     std::size_t& out) noexcept {
    std::size_t t = 0;
    if (!checked_add(image_header_bytes, 8, t)) return false;
    if (!checked_add(t, attr_len, t)) return false;
    if (!checked_add(t, pixel_bytes, t)) return false;
    if (t > kMaxImageFrameBytes) return false;
    out = t;
    return true;
}

// ACQUISITION payload: traj (traj_dims * samples * 4) + samples
// (samples * channels * 8), overflow-checked and capped at
// kMaxImageFrameBytes. All three header fields are uint16 so the products
// cannot overflow size_t, but the checked path keeps this true if the
// wire format ever widens — and the cap is the real defence: max fields
// give ~34 GiB. Used for scanner-origin and recon-returned acquisitions.
inline bool compute_acquisition_payload_bytes(std::size_t traj_dims,
                                              std::size_t samples,
                                              std::size_t channels,
                                              std::size_t& traj_bytes,
                                              std::size_t& sample_bytes) noexcept {
    std::size_t t = 0, s = 0, total = 0;
    if (!checked_mul(traj_dims, samples, t)) return false;
    if (!checked_mul(t, sizeof(float), t)) return false;
    if (!checked_mul(samples, channels, s)) return false;
    if (!checked_mul(s, 8 /* complex<float> */, s)) return false;
    if (!checked_add(t, s, total)) return false;
    if (total > kMaxImageFrameBytes) return false;
    traj_bytes = t;
    sample_bytes = s;
    return true;
}

// WAVEFORM payload: samples * channels * 4 bytes, overflow-checked and
// capped at kMaxImageFrameBytes. Used for scanner-origin and recon-returned
// waveforms (audit 2026-08-28 #4: neither was capped).
inline bool compute_waveform_data_bytes(std::size_t samples,
                                        std::size_t channels,
                                        std::size_t& out) noexcept {
    std::size_t d = 0;
    if (!checked_mul(samples, channels, d)) return false;
    if (!checked_mul(d, sizeof(std::uint32_t), d)) return false;
    if (d > kMaxImageFrameBytes) return false;
    out = d;
    return true;
}

// Validate a length-prefix body size: 4 (length field) + len, with cap.
inline bool validate_len_prefix_body(std::uint32_t len, std::size_t& out) noexcept {
    if (len > kMaxLenPrefixBodyBytes) return false;
    std::size_t total = 0;
    if (!checked_add(4, len, total)) return false;
    out = total;
    return true;
}

} // namespace mrd
