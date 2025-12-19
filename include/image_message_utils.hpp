#pragma once

#include <ismrmrd/ismrmrd.h>
#include <ismrmrd/xml.h>
#include <cstring>
#include <fstream>
#include <vector>
#include <filesystem>
#include <system_error>
#include <stdexcept>

// Generates an ISMRMRD image message file (Header + Data) at the specified path.
// Creates a 4x4 float32 ramp image.
inline void generate_image_message(const std::string& out_path_str) {
    std::filesystem::path out_path{out_path_str};
    if (out_path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(out_path.parent_path(), ec);
        if (ec) {
            throw std::runtime_error("Failed to create directory for " + out_path_str + ": " + ec.message());
        }
    }

    uint16_t nx = 4, ny = 4, nz = 1, channels = 1;
    ISMRMRD::Image<float> img(nx, ny, nz, channels);

    float* p = img.getDataPtr();
    size_t nelem = static_cast<size_t>(nx) * ny * nz * channels;
    for (size_t i = 0; i < nelem; ++i) p[i] = static_cast<float>(i);

    ISMRMRD::ImageHeader& h = img.getHead();
    h.channels   = channels;
    h.data_type  = ISMRMRD::ISMRMRD_FLOAT;
    h.matrix_size[0] = nx;
    h.matrix_size[1] = ny;
    h.matrix_size[2] = nz;
    h.field_of_view[0] = 1.f;
    h.field_of_view[1] = 1.f;
    h.field_of_view[2] = 1.f;

    std::ofstream ofs(out_path, std::ios::binary);
    if (!ofs) {
        throw std::runtime_error("Failed to open " + out_path_str + " for write");
    }
    ofs.write(reinterpret_cast<const char*>(&h), sizeof(ISMRMRD::ImageHeader));
    ofs.write(reinterpret_cast<const char*>(p), nelem * sizeof(float));
    ofs.close();
}
