#include <catch2/catch_all.hpp>
#include <filesystem>
#include <ismrmrd/dataset.h>
#include <ismrmrd/ismrmrd.h>
#include "mk_mrd_utils.hpp"
#include <random>

namespace fs = std::filesystem;

static std::string unique_temp_dir()
{
    auto base = fs::temp_directory_path();
    std::string name = "cwru_mk_mrd_test_" + std::to_string(std::random_device{}());
    fs::path full = base / name;
    fs::create_directories(full);
    return full.string();
}

TEST_CASE("mk_mrd generates valid ISMRMRD file", "[mk_mrd]")
{
    std::string temp = unique_temp_dir();
    fs::path mrd_path = fs::path(temp) / "test.mrd";

    generate_minimal_mrd(mrd_path.string());

    REQUIRE(fs::exists(mrd_path));

    ISMRMRD::Dataset d(mrd_path.c_str(), "dataset", false);
    
    std::string xml;
    d.readHeader(xml);
    CHECK(xml.find("ismrmrdHeader") != std::string::npos);
    CHECK(xml.find("123000000") != std::string::npos);

    uint32_t acq_count = d.getNumberOfAcquisitions();
    CHECK(acq_count == 1);

    ISMRMRD::Acquisition acq;
    d.readAcquisition(0, acq);
    CHECK(acq.number_of_samples() == 1);
    CHECK(acq.active_channels() == 1);

    fs::remove_all(temp);
}