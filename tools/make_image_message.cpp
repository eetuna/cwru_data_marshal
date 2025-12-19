// tools/make_image_message.cpp
#include <iostream>
#include <cstring>
#include <filesystem>
#include "image_message_utils.hpp"

int main(int argc, char** argv) {
    const char* out = "data/image_message.bin";

    // Allow optional --out <path>
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--out") == 0) {
            out = argv[i + 1];
        }
    }

    try {
        generate_image_message(out);
        
        // Calculate size for logging (4x4x1x1 float32 = 16 * 4 = 64 bytes data)
        size_t data_size = 4 * 4 * 1 * 1 * sizeof(float);
        std::cout << "Wrote " << out << " ("
                  << sizeof(ISMRMRD::ImageHeader) + data_size
                  << " bytes)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "make_image_message error: " << e.what() << "\n";
        return 1;
    }
}
