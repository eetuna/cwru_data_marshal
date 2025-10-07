#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <filesystem>
#include <vector>
#include <random>
#include <string>
#include <hdf5.h>

#include "mrd_sink.hpp"
#include "mrd_io.hpp"

namespace fs = std::filesystem;

static std::string unique_temp_dir()
{
    auto base = fs::temp_directory_path();
    std::string name = "cwru_marshal_test_" + std::to_string(std::random_device{}());
    fs::path full = base / name;
    fs::create_directories(full);
    return full.string();
}

TEST_CASE("MRD sink appends frames visible to SWMR readers", "[mrd]")
{
    std::string temp = unique_temp_dir();
    fs::path data_dir = fs::path(temp) / "data";
    fs::create_directories(data_dir / "mrd");

    MarshalState state;
    state.data_dir = data_dir.string();
    state.sink_mode = SinkMode::MRD;
    state.ws_emit = [](const std::string &) {};
    state.ws_emit_topic = [](const std::string &, const std::string &) {};

    mrd::MrdSink sink(state);

    mrd::ImageDimensions dims;
    dims.spatial = {4, 3, 1};
    dims.channels = 2;

    const size_t elements = static_cast<size_t>(dims.spatial[0]) * dims.spatial[1] * dims.channels;
    std::vector<float> frame1(elements, 1.5f);
    std::vector<float> frame2(elements, 2.5f);

    auto header_xml = mrd::default_ismrmrd_header(dims, mrd::ElementType::Float32, "streamA");
    auto result1 = sink.append_frame("streamA", dims, mrd::ElementType::Float32, header_xml, frame1.data(), frame1.size() * sizeof(float));
    REQUIRE(result1.frame_index == 0);

    auto result2 = sink.append_frame("streamA", dims, mrd::ElementType::Float32, header_xml, frame2.data(), frame2.size() * sizeof(float));
    REQUIRE(result2.frame_index == 1);
    REQUIRE(result1.file_path == result2.file_path);

    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
    REQUIRE(fapl >= 0);
    H5Pset_libver_bounds(fapl, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
    H5Pset_fclose_degree(fapl, H5F_CLOSE_SEMI);
    hid_t file = H5Fopen(result2.file_path.c_str(), H5F_ACC_RDONLY | H5F_ACC_SWMR_READ, fapl);
    H5Pclose(fapl);
    REQUIRE(file >= 0);

    hid_t dset = H5Dopen2(file, "/images/data", H5P_DEFAULT);
    REQUIRE(dset >= 0);
    REQUIRE(H5Drefresh(dset) >= 0);
    hid_t space = H5Dget_space(dset);
    REQUIRE(space >= 0);
    hsize_t dims_out[5] = {0};
    H5Sget_simple_extent_dims(space, dims_out, nullptr);
    CHECK(dims_out[0] == 2);
    CHECK(dims_out[1] == dims.channels);
    CHECK(dims_out[3] == dims.spatial[1]);
    CHECK(dims_out[4] == dims.spatial[0]);

    std::vector<float> readback(elements * 2);
    hid_t memspace = H5Screate_simple(5, dims_out, nullptr);
    REQUIRE(memspace >= 0);
    REQUIRE(H5Dread(dset, H5T_IEEE_F32LE, memspace, space, H5P_DEFAULT, readback.data()) >= 0);
    H5Sclose(memspace);
    H5Sclose(space);
    H5Dclose(dset);

    hid_t header_dset = H5Dopen2(file, "/header", H5P_DEFAULT);
    REQUIRE(header_dset >= 0);
    hid_t header_type = H5Dget_type(header_dset);
    REQUIRE(header_type >= 0);
    size_t header_size = H5Tget_size(header_type);
    std::vector<char> xml(header_size, '\0');
    REQUIRE(H5Dread(header_dset, header_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, xml.data()) >= 0);
    H5Tclose(header_type);
    H5Dclose(header_dset);

    H5Fclose(file);

    for (size_t i = 0; i < elements; ++i)
    {
        CHECK(readback[i] == Catch::Approx(frame1[i]));
        CHECK(readback[i + elements] == Catch::Approx(frame2[i]));
    }
    CHECK(std::string(xml.data()) == header_xml);
}
