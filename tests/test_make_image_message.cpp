#include <catch2/catch_all.hpp>
#include <filesystem>
#include <fstream>
#include <vector>
#include <ismrmrd/ismrmrd.h>
#include "image_message_utils.hpp"
#include <random>

namespace fs = std::filesystem;

static std::string unique_temp_dir()
{
    auto base = fs::temp_directory_path();
    std::string name = "cwru_make_image_test_" + std::to_string(std::random_device{}());
    fs::path full = base / name;
    fs::create_directories(full);
    return full.string();
}

TEST_CASE("make_image_message generates valid binary payload", "[tools]")
{
    std::string temp = unique_temp_dir();
    fs::path bin_path = fs::path(temp) / "image_msg.bin";

    generate_image_message(bin_path.string());

    REQUIRE(fs::exists(bin_path));
    
    // Check file size: Header + 4x4 float32
    size_t expected_data_size = 4 * 4 * sizeof(float);
    size_t expected_total_size = sizeof(ISMRMRD::ImageHeader) + expected_data_size;
    REQUIRE(fs::file_size(bin_path) == expected_total_size);

    // Read back and verify header
    std::ifstream ifs(bin_path, std::ios::binary);
    ISMRMRD::ImageHeader h;
    ifs.read(reinterpret_cast<char*>(&h), sizeof(h));
    
    CHECK(h.channels == 1);
    CHECK(h.data_type == ISMRMRD::ISMRMRD_FLOAT);
    CHECK(h.matrix_size[0] == 4);
    CHECK(h.matrix_size[1] == 4);

    // Verify data (ramp)
    std::vector<float> data(16);
    ifs.read(reinterpret_cast<char*>(data.data()), 16 * sizeof(float));
    CHECK(data[0] == 0.0f);
    CHECK(data[1] == 1.0f);
    CHECK(data[15] == 15.0f);

    fs::remove_all(temp);
}
