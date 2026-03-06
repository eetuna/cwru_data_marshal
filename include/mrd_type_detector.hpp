/*
 * File: include/mrd_type_detector.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Detect ISMRMRD data types (raw k-space vs reconstructed images)
 *
 * This module provides automatic detection of MRD data format to enable
 * intelligent routing of raw k-space data to reconstruction services.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ismrmrd/ismrmrd.h>

namespace mrd
{

/**
 * Enumeration of supported MRD data types
 */
enum class MrdDataType
{
    ACQUISITION,  ///< Raw k-space data (AcquisitionHeader)
    IMAGE,        ///< Reconstructed image data (ImageHeader)
    HDF5_FILE,    ///< Complete ISMRMRD HDF5 file
    UNKNOWN       ///< Unknown or invalid format
};

/**
 * Convert MrdDataType enum to string for logging
 */
inline const char* mrd_data_type_to_string(MrdDataType type)
{
    switch (type)
    {
    case MrdDataType::ACQUISITION:
        return "ACQUISITION";
    case MrdDataType::IMAGE:
        return "IMAGE";
    case MrdDataType::HDF5_FILE:
        return "HDF5_FILE";
    case MrdDataType::UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

/**
 * Check if data starts with HDF5 magic signature
 * HDF5 signature: \x89 H D F \r \n \x1a \n
 */
inline bool is_hdf5_signature(const void* data, size_t size)
{
    if (size < 8)
        return false;

    const uint8_t hdf5_sig[] = {0x89, 0x48, 0x44, 0x46, 0x0d, 0x0a, 0x1a, 0x0a};
    return std::memcmp(data, hdf5_sig, 8) == 0;
}

/**
 * Heuristic check if data contains ISMRMRD AcquisitionHeader
 *
 * AcquisitionHeader characteristics:
 * - number_of_samples: typically 128-2048 for k-space readouts
 * - active_channels: typically 1-128 for coil arrays
 * - trajectory_dimensions: 0-3 (most common: 0 or 2)
 *
 * References:
 * - https://ismrmrd.github.io/apidocs/1.5.0/struct_i_s_m_r_m_r_d_1_1_i_s_m_r_m_r_d___acquisition_header.html
 */
inline bool is_acquisition_header(const void* data, size_t size)
{
    if (size < sizeof(ISMRMRD::AcquisitionHeader))
        return false;

    const auto* hdr = static_cast<const ISMRMRD::AcquisitionHeader*>(data);

    // Check version field (should be 1 or 2 for ISMRMRD)
    if (hdr->version == 0 || hdr->version > 10)
        return false;

    // Check number_of_samples (typical range for k-space lines)
    if (hdr->number_of_samples == 0 || hdr->number_of_samples > 16384)
        return false;

    // Check active_channels (typical range for MRI coils)
    if (hdr->active_channels == 0 || hdr->active_channels > 128)
        return false;

    // Check trajectory_dimensions (0=Cartesian, 1-3=non-Cartesian)
    if (hdr->trajectory_dimensions > 3)
        return false;

    // Additional sanity check: available_channels should be >= active_channels
    if (hdr->available_channels < hdr->active_channels)
        return false;

    return true;
}

/**
 * Heuristic check if data contains ISMRMRD ImageHeader
 *
 * ImageHeader characteristics:
 * - matrix_size[0-2]: typically 16-2048 for medical images
 * - channels: typically 1-128 for multi-channel images
 * - data_type: valid ISMRMRD data type enum (1-10)
 *
 * References:
 * - https://ismrmrd.readthedocs.io/en/latest/mrd_raw_data.html
 */
inline bool is_image_header(const void* data, size_t size)
{
    if (size < sizeof(ISMRMRD::ImageHeader))
        return false;

    const auto* hdr = static_cast<const ISMRMRD::ImageHeader*>(data);

    // Check version field (should be 1 or 2 for ISMRMRD)
    if (hdr->version == 0 || hdr->version > 10)
        return false;

    // Check matrix_size (typical range for medical images)
    // At least X and Y dimensions should be non-zero
    if (hdr->matrix_size[0] == 0 || hdr->matrix_size[0] > 4096)
        return false;
    if (hdr->matrix_size[1] == 0 || hdr->matrix_size[1] > 4096)
        return false;
    // Z dimension can be 0 for 2D images
    if (hdr->matrix_size[2] > 4096)
        return false;

    // Check channels (typical range for image channels)
    if (hdr->channels == 0 || hdr->channels > 128)
        return false;

    // Check data_type (should be valid ISMRMRD data type)
    // ISMRMRD data types: 1=USHORT, 2=SHORT, 3=UINT, 4=INT, 5=FLOAT, 6=DOUBLE, 7=CXFLOAT, 8=CXDOUBLE
    if (hdr->data_type == 0 || hdr->data_type > 10)
        return false;

    return true;
}

/**
 * Detect the type of MRD data from binary buffer
 *
 * Detection order:
 * 1. Check for HDF5 file signature (most distinctive)
 * 2. Check for AcquisitionHeader (raw k-space)
 * 3. Check for ImageHeader (reconstructed image)
 * 4. Return UNKNOWN if none match
 *
 * @param data Pointer to binary data buffer
 * @param size Size of data buffer in bytes
 * @return MrdDataType enum indicating detected type
 *
 * Example usage:
 * @code
 * const std::string& body = req.body();
 * MrdDataType type = detect_mrd_type(body.data(), body.size());
 *
 * switch (type) {
 *     case MrdDataType::ACQUISITION:
 *         // Handle raw k-space data
 *         break;
 *     case MrdDataType::IMAGE:
 *         // Handle reconstructed image
 *         break;
 *     case MrdDataType::HDF5_FILE:
 *         // Handle complete file
 *         break;
 *     default:
 *         // Error: unknown format
 *         break;
 * }
 * @endcode
 */
inline MrdDataType detect_mrd_type(const void* data, size_t size)
{
    // Minimum size check
    if (data == nullptr || size < 8)
        return MrdDataType::UNKNOWN;

    // Check for HDF5 file (most distinctive signature)
    if (is_hdf5_signature(data, size))
        return MrdDataType::HDF5_FILE;

    // Try AcquisitionHeader first (raw k-space is more common in streaming)
    // Note: is_acquisition_header has its own size check internally
    if (is_acquisition_header(data, size))
        return MrdDataType::ACQUISITION;

    // Try ImageHeader (reconstructed data)
    // Note: ImageHeader (198 bytes) is smaller than AcquisitionHeader (340 bytes)
    if (is_image_header(data, size))
        return MrdDataType::IMAGE;

    return MrdDataType::UNKNOWN;
}

} // namespace mrd
