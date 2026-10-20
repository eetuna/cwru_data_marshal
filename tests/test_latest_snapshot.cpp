/*
 * Tests for the viewer snapshot (latest_image_writer.hpp): the file holds
 * the newest image per slice position, whatever the recon's numbering.
 */

#include <catch2/catch_all.hpp>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <random>
#include <thread>

#include <ismrmrd/dataset.h>
#include <ismrmrd/ismrmrd.h>

#include "latest_image_writer.hpp"
#include "mrd_stream_tags.hpp"

namespace fs = std::filesystem;

static const std::string XML = R"(<?xml version="1.0"?>
<ismrmrdHeader xmlns="http://www.ismrmrd.org/ISMRMRD">
<encoding><encodedSpace><matrixSize><x>4</x><y>4</y><z>1</z></matrixSize></encodedSpace></encoding>
</ismrmrdHeader>)";

static fs::path temp_dir() {
    auto d = fs::temp_directory_path() / ("cwru_snapshot_test_" + std::to_string(std::random_device{}()));
    fs::create_directories(d);
    return d;
}

struct Img {
    float z_mm = 0;
    float value = 1;
    uint16_t n = 4;
    uint16_t type = ISMRMRD::ISMRMRD_IMTYPE_MAGNITUDE;
    uint16_t slice = 0;
};

static std::vector<uint8_t> wire(const Img& i) {
    ISMRMRD::ImageHeader h{};
    h.version = 1;
    h.data_type = ISMRMRD::ISMRMRD_FLOAT;
    h.matrix_size[0] = i.n;
    h.matrix_size[1] = i.n;
    h.matrix_size[2] = 1;
    h.channels = 1;
    h.image_type = i.type;
    h.slice = i.slice;
    h.position[2] = i.z_mm;
    h.read_dir[0] = 1;
    h.phase_dir[1] = 1;
    h.slice_dir[2] = 1;
    const uint64_t attr_len = 0;
    std::vector<float> px(size_t(i.n) * i.n, i.value);
    std::vector<uint8_t> b(mrd::IMAGE_HEADER_BYTES + 8 + px.size() * sizeof(float));
    std::memcpy(b.data(), &h, mrd::IMAGE_HEADER_BYTES);
    std::memcpy(b.data() + mrd::IMAGE_HEADER_BYTES, &attr_len, 8);
    std::memcpy(b.data() + mrd::IMAGE_HEADER_BYTES + 8, px.data(), px.size() * sizeof(float));
    return b;
}

static void add(mrd::LatestSnapshot& s, const Img& i) {
    auto b = wire(i);
    s.add_image(b.data(), b.size());
}

static mrd::LatestSnapshot::View wait_for(const mrd::LatestSnapshot& s, size_t images) {
    for (int i = 0; i < 400; ++i) {
        auto v = s.view();
        if (!v.path.empty() && v.images == images) return v;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return s.view();
}

// (z, value) of every image in variable `var`, in file order.
static std::vector<std::pair<float, float>> read_var(const std::string& path, const std::string& var) {
    ISMRMRD::Dataset ds(path.c_str(), "/dataset", false);
    std::vector<std::pair<float, float>> out;
    for (uint32_t k = 0; k < ds.getNumberOfImages(var); ++k) {
        ISMRMRD::Image<float> im;
        ds.readImage(var, k, im);
        out.emplace_back(im.getHead().position[2], im.getDataPtr()[0]);
    }
    return out;
}

TEST_CASE("Newest image per position replaces, never accumulates", "[snapshot]") {
    mrd::LatestSnapshot snap(temp_dir());
    snap.begin_scan(XML);
    for (float z : {16.f, 0.f, 8.f}) add(snap, {z, 1});
    auto v = wait_for(snap, 3);
    REQUIRE(v.images == 3);
    // Ordered along the slice normal, whatever the arrival order.
    auto imgs = read_var(v.path, "image_0");
    REQUIRE(imgs.size() == 3);
    CHECK(imgs[0].first == 0.f);
    CHECK(imgs[1].first == 8.f);
    CHECK(imgs[2].first == 16.f);

    add(snap, {8.f, 2});   // newer image at 8 mm
    for (int i = 0; i < 400 && read_var(snap.view().path, "image_0")[1].second != 2.f; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    imgs = read_var(snap.view().path, "image_0");
    REQUIRE(imgs.size() == 3);
    CHECK(imgs[1] == std::make_pair(8.f, 2.f));
}

TEST_CASE("Slice numbering does not matter, positions do", "[snapshot]") {
    mrd::LatestSnapshot snap(temp_dir());
    snap.begin_scan(XML);
    for (float z : {0.f, 8.f, 16.f}) add(snap, {z, 1, 4, ISMRMRD::ISMRMRD_IMTYPE_MAGNITUDE, /*slice=*/0});
    CHECK(wait_for(snap, 3).images == 3);
}

TEST_CASE("Magnitude and phase at one position are kept apart", "[snapshot]") {
    mrd::LatestSnapshot snap(temp_dir());
    snap.begin_scan(XML);
    add(snap, {0.f, 1, 4, ISMRMRD::ISMRMRD_IMTYPE_MAGNITUDE});
    add(snap, {0.f, 1, 4, ISMRMRD::ISMRMRD_IMTYPE_PHASE});
    CHECK(wait_for(snap, 2).images == 2);
}

TEST_CASE("Images of different sizes are all written", "[snapshot]") {
    mrd::LatestSnapshot snap(temp_dir());
    snap.begin_scan(XML);
    add(snap, {0.f, 1, 4});
    add(snap, {8.f, 1, 8});
    auto v = wait_for(snap, 2);
    REQUIRE(v.images == 2);
    CHECK(read_var(v.path, "image_0").size() == 1);
    CHECK(read_var(v.path, "image_1").size() == 1);
}

TEST_CASE("A new scan starts empty and bumps the generation", "[snapshot]") {
    mrd::LatestSnapshot snap(temp_dir());
    snap.begin_scan(XML);
    add(snap, {0.f, 1});
    auto v = wait_for(snap, 1);
    REQUIRE(v.images == 1);
    snap.begin_scan(XML);
    auto after = snap.view();
    CHECK(after.path.empty());
    CHECK(after.images == 0);
    CHECK(after.generation > v.generation);
}
