/*
 * File: include/mrd_stream_tags.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Single source of MRD wire-protocol message type constants
 *
 * Values match python-ismrmrd-server constants.py exactly.
 * Every module that needs these constants includes this one header.
 */

#pragma once

#include <cstdint>

namespace mrd {

// Wire-protocol message identifiers (uint16_t tag preceding each message)
// From python-ismrmrd-server constants.py
constexpr uint16_t MRD_MESSAGE_CONFIG_FILE         = 1;
constexpr uint16_t MRD_MESSAGE_CONFIG_TEXT          = 2;
constexpr uint16_t MRD_MESSAGE_METADATA_XML_TEXT    = 3;
constexpr uint16_t MRD_MESSAGE_CLOSE                = 4;
constexpr uint16_t MRD_MESSAGE_TEXT                  = 5;
constexpr uint16_t MRD_MESSAGE_ISMRMRD_ACQUISITION  = 1008;
constexpr uint16_t MRD_MESSAGE_ISMRMRD_IMAGE        = 1022;
constexpr uint16_t MRD_MESSAGE_ISMRMRD_WAVEFORM     = 1026;

// Wire-format struct sizes (from libismrmrd static_asserts)
constexpr size_t ACQUISITION_HEADER_BYTES = 340;
constexpr size_t IMAGE_HEADER_BYTES       = 198;
constexpr size_t WAVEFORM_HEADER_BYTES    = 40;

} // namespace mrd
