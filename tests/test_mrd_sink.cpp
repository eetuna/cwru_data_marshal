#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <filesystem>
#include <vector>
#include <random>
#include <string>
#include <cstdint>
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

struct Complex32
{
    float r;
    float i;
};

TEST_CASE("MRD sink handles complex64 payloads", "[mrd][complex]")
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
    dims.spatial = {2, 2, 1};
    dims.channels = 1;

    const size_t elements = static_cast<size_t>(dims.spatial[0]) * dims.spatial[1] * dims.channels;
    std::vector<Complex32> frame(elements);
    for (size_t i = 0; i < elements; ++i)
    {
        frame[i].r = static_cast<float>(i);
        frame[i].i = static_cast<float>(i + 10);
    }

    auto header_xml = mrd::default_ismrmrd_header(dims, mrd::ElementType::ComplexFloat32, "streamC");
    auto result = sink.append_frame("streamC", dims, mrd::ElementType::ComplexFloat32, header_xml, frame.data(), frame.size() * sizeof(Complex32));
    REQUIRE(result.frame_index == 0);

    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
    REQUIRE(fapl >= 0);
    H5Pset_libver_bounds(fapl, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
    H5Pset_fclose_degree(fapl, H5F_CLOSE_SEMI);
    hid_t file = H5Fopen(result.file_path.c_str(), H5F_ACC_RDONLY | H5F_ACC_SWMR_READ, fapl);
    H5Pclose(fapl);
    REQUIRE(file >= 0);

    hid_t dset = H5Dopen2(file, "/images/data", H5P_DEFAULT);
    REQUIRE(dset >= 0);
    REQUIRE(H5Drefresh(dset) >= 0);
    hid_t space = H5Dget_space(dset);
    REQUIRE(space >= 0);
    hsize_t dims_out[5] = {0};
    H5Sget_simple_extent_dims(space, dims_out, nullptr);
    CHECK(dims_out[0] == 1);
    CHECK(dims_out[1] == dims.channels);
    CHECK(dims_out[3] == dims.spatial[1]);
    CHECK(dims_out[4] == dims.spatial[0]);

    std::vector<Complex32> readback(elements);
    hsize_t read_dims[5] = {1, dims.channels, dims.spatial[2], dims.spatial[1], dims.spatial[0]};
    hid_t memspace = H5Screate_simple(5, read_dims, nullptr);
    REQUIRE(memspace >= 0);
    hid_t memtype = H5Tcreate(H5T_COMPOUND, sizeof(Complex32));
    REQUIRE(memtype >= 0);
    REQUIRE(H5Tinsert(memtype, "r", HOFFSET(Complex32, r), H5T_IEEE_F32LE) >= 0);
    REQUIRE(H5Tinsert(memtype, "i", HOFFSET(Complex32, i), H5T_IEEE_F32LE) >= 0);
    REQUIRE(H5Dread(dset, memtype, memspace, space, H5P_DEFAULT, readback.data()) >= 0);
    H5Tclose(memtype);
    H5Sclose(memspace);
    H5Sclose(space);
    H5Dclose(dset);

    H5Fclose(file);

    for (size_t i = 0; i < elements; ++i)
    {
        CHECK(readback[i].r == Catch::Approx(frame[i].r));
        CHECK(readback[i].i == Catch::Approx(frame[i].i));
    }
}

TEST_CASE("MRD sink handles int16 payloads", "[mrd][int16]")
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
    dims.spatial = {3, 2, 1};
    dims.channels = 1;

    const size_t elements = static_cast<size_t>(dims.spatial[0]) * dims.spatial[1] * dims.channels;
    std::vector<int16_t> frame(elements);
    for (size_t i = 0; i < elements; ++i)
        frame[i] = static_cast<int16_t>(i * 7 - 3);

    auto header_xml = mrd::default_ismrmrd_header(dims, mrd::ElementType::Int16, "streamI16");
    auto result = sink.append_frame("streamI16", dims, mrd::ElementType::Int16, header_xml, frame.data(), frame.size() * sizeof(int16_t));
    REQUIRE(result.frame_index == 0);

    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
    REQUIRE(fapl >= 0);
    H5Pset_libver_bounds(fapl, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
    H5Pset_fclose_degree(fapl, H5F_CLOSE_SEMI);
    hid_t file = H5Fopen(result.file_path.c_str(), H5F_ACC_RDONLY | H5F_ACC_SWMR_READ, fapl);
    H5Pclose(fapl);
    REQUIRE(file >= 0);

    hid_t dset = H5Dopen2(file, "/images/data", H5P_DEFAULT);
    REQUIRE(dset >= 0);
    REQUIRE(H5Drefresh(dset) >= 0);

    hid_t space = H5Dget_space(dset);
    REQUIRE(space >= 0);
    hsize_t dims_out[5] = {0};
    H5Sget_simple_extent_dims(space, dims_out, nullptr);
    CHECK(dims_out[0] == 1);
    CHECK(dims_out[1] == dims.channels);
    CHECK(dims_out[3] == dims.spatial[1]);
    CHECK(dims_out[4] == dims.spatial[0]);

    std::vector<int16_t> readback(elements);
    hid_t memspace = H5Screate_simple(5, dims_out, nullptr);
    REQUIRE(memspace >= 0);
    REQUIRE(H5Dread(dset, H5T_STD_I16LE, memspace, space, H5P_DEFAULT, readback.data()) >= 0);
    H5Sclose(memspace);
    H5Sclose(space);
    H5Dclose(dset);
    H5Fclose(file);

    for (size_t i = 0; i < elements; ++i)
        CHECK(readback[i] == frame[i]);
}

TEST_CASE("MRD sink rolls files when dimensions change", "[mrd][rollover]")
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

    mrd::ImageDimensions dims1;
    dims1.spatial = {4, 4, 2};
    dims1.channels = 1;

    const size_t vox1 = static_cast<size_t>(dims1.spatial[0]) * dims1.spatial[1] * dims1.spatial[2] * dims1.channels;
    std::vector<float> frame1(vox1, 1.0f);
    auto header1 = mrd::default_ismrmrd_header(dims1, mrd::ElementType::Float32, "shapeStream");

    auto result1 = sink.append_frame("shapeStream", dims1, mrd::ElementType::Float32, header1, frame1.data(),
                                     frame1.size() * sizeof(float));

    mrd::ImageDimensions dims2;
    dims2.spatial = {8, 8, 3};
    dims2.channels = 1;
    const size_t vox2 = static_cast<size_t>(dims2.spatial[0]) * dims2.spatial[1] * dims2.spatial[2] * dims2.channels;
    std::vector<float> frame2(vox2, 2.0f);
    auto header2 = mrd::default_ismrmrd_header(dims2, mrd::ElementType::Float32, "shapeStream");

    auto result2 = sink.append_frame("shapeStream", dims2, mrd::ElementType::Float32, header2, frame2.data(),
                                     frame2.size() * sizeof(float));

    REQUIRE(result1.file_path != result2.file_path);
    REQUIRE(result1.frame_index == 0);
    REQUIRE(result2.frame_index == 0);

    auto name1 = result1.file_path.filename().string();
    auto name2 = result2.file_path.filename().string();
    CHECK(name1.find("4x4x2") != std::string::npos);
    CHECK(name2.find("8x8x3") != std::string::npos);

    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
    REQUIRE(fapl >= 0);
    H5Pset_libver_bounds(fapl, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
    H5Pset_fclose_degree(fapl, H5F_CLOSE_SEMI);

    hid_t file1 = H5Fopen(result1.file_path.c_str(), H5F_ACC_RDONLY | H5F_ACC_SWMR_READ, fapl);
    REQUIRE(file1 >= 0);
    hid_t dset1 = H5Dopen2(file1, "/images/data", H5P_DEFAULT);
    REQUIRE(dset1 >= 0);
    hid_t space1 = H5Dget_space(dset1);
    REQUIRE(space1 >= 0);
    hsize_t dims_out1[5] = {0};
    H5Sget_simple_extent_dims(space1, dims_out1, nullptr);
    CHECK(dims_out1[0] == 1);
    CHECK(dims_out1[2] == dims1.spatial[2]);
    CHECK(dims_out1[3] == dims1.spatial[1]);
    CHECK(dims_out1[4] == dims1.spatial[0]);
    H5Sclose(space1);
    H5Dclose(dset1);
    H5Fclose(file1);

    hid_t file2 = H5Fopen(result2.file_path.c_str(), H5F_ACC_RDONLY | H5F_ACC_SWMR_READ, fapl);
    H5Pclose(fapl);
    REQUIRE(file2 >= 0);
    hid_t dset2 = H5Dopen2(file2, "/images/data", H5P_DEFAULT);
    REQUIRE(dset2 >= 0);
    hid_t space2 = H5Dget_space(dset2);
    REQUIRE(space2 >= 0);
    hsize_t dims_out2[5] = {0};
    H5Sget_simple_extent_dims(space2, dims_out2, nullptr);
    CHECK(dims_out2[0] == 1);
    CHECK(dims_out2[2] == dims2.spatial[2]);
    CHECK(dims_out2[3] == dims2.spatial[1]);
    CHECK(dims_out2[4] == dims2.spatial[0]);
    H5Sclose(space2);
    H5Dclose(dset2);
    H5Fclose(file2);
}

TEST_CASE("MRD dataset chunk size adapts to frame shape", "[mrd][chunk]")
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
    dims.spatial = {512, 512, 20};
    dims.channels = 1;

    const size_t voxels = static_cast<size_t>(dims.spatial[0]) * dims.spatial[1] * dims.spatial[2] * dims.channels;
    std::vector<float> frame(voxels, 0.5f);
    auto header = mrd::default_ismrmrd_header(dims, mrd::ElementType::Float32, "chunky");

    auto result = sink.append_frame("chunky", dims, mrd::ElementType::Float32, header, frame.data(),
                                    frame.size() * sizeof(float));

    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
    REQUIRE(fapl >= 0);
    H5Pset_libver_bounds(fapl, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
    H5Pset_fclose_degree(fapl, H5F_CLOSE_SEMI);
    hid_t file = H5Fopen(result.file_path.c_str(), H5F_ACC_RDONLY | H5F_ACC_SWMR_READ, fapl);
    H5Pclose(fapl);
    REQUIRE(file >= 0);

    hid_t dset = H5Dopen2(file, "/images/data", H5P_DEFAULT);
    REQUIRE(dset >= 0);
    hid_t dcpl = H5Dget_create_plist(dset);
    REQUIRE(dcpl >= 0);
    REQUIRE(H5Pget_layout(dcpl) == H5D_CHUNKED);

    hsize_t chunk[5] = {0};
    REQUIRE(H5Pget_chunk(dcpl, 5, chunk) >= 0);
    H5Pclose(dcpl);
    H5Dclose(dset);
    H5Fclose(file);

    constexpr unsigned long long target = 8ULL * 1024ULL * 1024ULL;
    unsigned long long chunk_bytes = static_cast<unsigned long long>(chunk[0]) * static_cast<unsigned long long>(chunk[1]) *
                                     static_cast<unsigned long long>(chunk[2]) * static_cast<unsigned long long>(chunk[3]) *
                                     static_cast<unsigned long long>(chunk[4]) * sizeof(float);

    CHECK(chunk[0] == 1);
    CHECK(chunk[1] == dims.channels);
    CHECK(chunk[2] <= dims.spatial[2]);
    CHECK(chunk[3] <= dims.spatial[1]);
    CHECK(chunk[4] <= dims.spatial[0]);
    CHECK(chunk_bytes > 0);
    CHECK(chunk_bytes <= target);
}
