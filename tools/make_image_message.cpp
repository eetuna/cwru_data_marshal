// tools/make_image_message.cpp
#include <ismrmrd/ismrmrd.h>
#include <ismrmrd/xml.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    const char* out = "image_message.bin";
    // Hardcode a tiny example: 4x4, 1 channel, float32
    uint16_t nx = 4, ny = 4, nz = 1, channels = 1;

    // Allow optional --out <path>
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--out") == 0) {
            out = argv[i + 1];
        }
    }

    // Construct an ISMRMRD Image<float> (float32 voxels)
    ISMRMRD::Image<float> img(nx, ny, nz, channels);

    // Fill data with a simple ramp
    float* p = img.getDataPtr();
    size_t nelem = static_cast<size_t>(nx) * ny * nz * channels;
    for (size_t i = 0; i < nelem; ++i) p[i] = static_cast<float>(i);

    // Minimal header fields
    ISMRMRD::ImageHeader& h = img.getHead();
    h.channels   = channels;
    h.data_type  = ISMRMRD::ISMRMRD_FLOAT; // float32
    h.matrix_size[0] = nx;
    h.matrix_size[1] = ny;
    h.matrix_size[2] = nz;
    h.field_of_view[0] = 1.f;
    h.field_of_view[1] = 1.f;
    h.field_of_view[2] = 1.f;

    // Write [header][raw voxels] to out
    std::ofstream ofs(out, std::ios::binary);
    if (!ofs) {
        std::cerr << "Failed to open " << out << " for write\n";
        return 1;
    }
    ofs.write(reinterpret_cast<const char*>(&h), sizeof(ISMRMRD::ImageHeader));
    ofs.write(reinterpret_cast<const char*>(p), nelem * sizeof(float));
    ofs.close();

    std::cout << "Wrote " << out << " ("
              << sizeof(ISMRMRD::ImageHeader) + nelem*sizeof(float)
              << " bytes)\n";
    return 0;
}
