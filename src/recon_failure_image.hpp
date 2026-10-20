/*
 * File: src/recon_failure_image.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: The "RECON FAILED" image the marshal sends the scanner when the
 *          reconstruction connection fails, so the operator sees it on the
 *          console and the scan still finalizes.
 *
 * Built like the reference recons build their images (python-ismrmrd-server
 * invertcontrast/simplefft): int16 pixels, geometry copied from the scan's
 * raw data, so the scanner places it like any other image of the scan.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <ismrmrd/ismrmrd.h>

#include "mrd_stream_tags.hpp"

namespace mrd {

// `geom`: the most recent acquisition header of the scan, or nullptr if the
// scan sent no raw data yet (the image then has no geometry).
inline std::vector<uint8_t> build_recon_failure_image_body(const ISMRMRD::AcquisitionHeader* geom)
{
    constexpr uint16_t nx = 128;
    constexpr uint16_t ny = 128;

    ISMRMRD::ImageHeader hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    hdr.version = 1;
    hdr.data_type = ISMRMRD::ISMRMRD_SHORT;
    hdr.matrix_size[0] = nx;
    hdr.matrix_size[1] = ny;
    hdr.matrix_size[2] = 1;
    hdr.channels = 1;
    hdr.field_of_view[0] = static_cast<float>(nx);
    hdr.field_of_view[1] = static_cast<float>(ny);
    hdr.field_of_view[2] = 1.0f;
    hdr.image_type = ISMRMRD::ISMRMRD_IMTYPE_MAGNITUDE;
    hdr.image_series_index = 9999;
    if (geom) {
        hdr.measurement_uid = geom->measurement_uid;
        for (int i = 0; i < 3; ++i) {
            hdr.position[i] = geom->position[i];
            hdr.read_dir[i] = geom->read_dir[i];
            hdr.phase_dir[i] = geom->phase_dir[i];
            hdr.slice_dir[i] = geom->slice_dir[i];
            hdr.patient_table_position[i] = geom->patient_table_position[i];
        }
        hdr.slice = geom->idx.slice;
    }

    const std::string attr =
        "<ismrmrdMeta><meta><name>DataRole</name><value>ReconFailure</value></meta>"
        "<meta><name>ErrorMessage</name><value>RECON FAILED</value></meta></ismrmrdMeta>";
    const uint64_t attr_len = static_cast<uint64_t>(attr.size());

    // Dark field with a bright border and an X.
    std::vector<int16_t> pixels(static_cast<size_t>(nx) * ny, 200);
    for (uint16_t y = 0; y < ny; ++y) {
        for (uint16_t x = 0; x < nx; ++x) {
            const bool border = x < 4 || y < 4 || x >= nx - 4 || y >= ny - 4;
            const bool diag = (x > y ? x - y : y - x) < 3;
            const bool anti = (x + y > nx - 3) && (x + y < nx + 3);
            if (border || diag || anti) pixels[static_cast<size_t>(y) * nx + x] = 4095;
        }
    }

    const size_t pixel_bytes = pixels.size() * sizeof(int16_t);
    std::vector<uint8_t> body(IMAGE_HEADER_BYTES + sizeof(uint64_t) + attr_len + pixel_bytes);
    size_t off = 0;
    std::memcpy(body.data() + off, &hdr, IMAGE_HEADER_BYTES);
    off += IMAGE_HEADER_BYTES;
    std::memcpy(body.data() + off, &attr_len, sizeof(uint64_t));
    off += sizeof(uint64_t);
    std::memcpy(body.data() + off, attr.data(), attr_len);
    off += attr_len;
    std::memcpy(body.data() + off, pixels.data(), pixel_bytes);
    return body;
}

} // namespace mrd
