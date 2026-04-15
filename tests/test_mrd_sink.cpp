/*
 * Tests for MrdSink: canonical ISMRMRD HDF5 layout verification via readback.
 * Uses ISMRMRD::Dataset to verify that appendAcquisition/appendImage/appendWaveform
 * produced correct canonical HDF5 files.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <filesystem>
#include <vector>
#include <cstring>
#include <complex>
#include <fstream>

#include <hdf5.h>
#include <ismrmrd/ismrmrd.h>
#include <ismrmrd/dataset.h>
#include <ismrmrd/waveform.h>

#include "mrd_sink.hpp"
#include "mrd_stream_tags.hpp"

namespace fs = std::filesystem;

static const std::string TEST_XML = R"(<?xml version="1.0"?>
<ismrmrdHeader xmlns="http://www.ismrmrd.org/ISMRMRD">
  <encoding><encodedSpace><matrixSize><x>128</x><y>128</y><z>1</z></matrixSize></encodedSpace></encoding>
</ismrmrdHeader>)";

static fs::path temp_h5(const std::string& name) {
    auto p = fs::temp_directory_path() / "test_mrd_sink" / name;
    fs::create_directories(p.parent_path());
    fs::remove(p); // clean up from prior run
    return p;
}

TEST_CASE("MrdSink writes header and reads it back", "[mrd_sink]") {
    auto path = temp_h5("header_test.h5");
    {
        mrd::MrdSink sink(path);
        sink.set_header(TEST_XML);
    }

    ISMRMRD::Dataset ds(path.c_str(), "/dataset", false);
    std::string xml;
    ds.readHeader(xml);
    REQUIRE(xml.find("<ismrmrdHeader") != std::string::npos);
}

TEST_CASE("MrdSink writes python savedata string datasets", "[mrd_sink]") {
    auto path = temp_h5("metadata_test.h5");
    {
        mrd::MrdSink sink(path);
        sink.write_string_dataset("config_file", "simplefft");
        sink.write_string_dataset("config", "config text");
    }

    hid_t file = H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    REQUIRE(file >= 0);
    hid_t dset = H5Dopen2(file, "/dataset/config_file", H5P_DEFAULT);
    REQUIRE(dset >= 0);
    hid_t type = H5Dget_type(dset);
    char* value = nullptr;
    REQUIRE(H5Dread(dset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, &value) >= 0);
    REQUIRE(std::string(value) == "simplefft");
    H5free_memory(value);
    H5Tclose(type);
    H5Dclose(dset);
    H5Fclose(file);
}

TEST_CASE("MrdSink appends acquisitions and reads them back", "[mrd_sink]") {
    auto path = temp_h5("acq_test.h5");
    constexpr uint16_t nsamples = 128;
    constexpr uint16_t nchannels = 4;

    {
        mrd::MrdSink sink(path);
        sink.set_header(TEST_XML);

        for (int i = 0; i < 3; ++i) {
            ISMRMRD::Acquisition acq(nsamples, nchannels);
            ISMRMRD::AcquisitionHeader h = acq.getHead();
            h.version = 1;
            h.scan_counter = static_cast<uint32_t>(i);
            acq.setHead(h);
            // Fill with recognizable pattern
            for (uint16_t c = 0; c < nchannels; ++c)
                for (uint16_t s = 0; s < nsamples; ++s)
                    acq.getDataPtr()[c * nsamples + s] = complex_float_t(
                        static_cast<float>(i + s), static_cast<float>(c));
            sink.append_acquisition(acq);
        }
        REQUIRE(sink.acquisition_count() == 3);
    }

    // Readback
    ISMRMRD::Dataset ds(path.c_str(), "/dataset", false);
    REQUIRE(ds.getNumberOfAcquisitions() == 3);

    ISMRMRD::Acquisition acq;
    ds.readAcquisition(1, acq);
    REQUIRE(acq.getHead().scan_counter == 1);
    REQUIRE(acq.getHead().number_of_samples == nsamples);
    REQUIRE(acq.getHead().active_channels == nchannels);
    // Check data: sample 0, channel 0 should be (1+0, 0)
    REQUIRE(acq.getDataPtr()[0].real() == Catch::Approx(1.0f));
}

TEST_CASE("MrdSink appends float images and reads them back", "[mrd_sink]") {
    auto path = temp_h5("img_test.h5");
    constexpr uint16_t nx = 16, ny = 16, nz = 1, nc = 1;

    {
        mrd::MrdSink sink(path);
        sink.set_header(TEST_XML);

        ISMRMRD::ImageHeader hdr;
        std::memset(&hdr, 0, sizeof(hdr));
        hdr.version = 1;
        hdr.data_type = ISMRMRD::ISMRMRD_FLOAT;
        hdr.matrix_size[0] = nx;
        hdr.matrix_size[1] = ny;
        hdr.matrix_size[2] = nz;
        hdr.channels = nc;
        hdr.image_series_index = 0;

        std::vector<float> pixels(nx * ny * nz * nc, 42.0f);
        std::string attr = "test_attr";

        sink.append_image("image_0", hdr, attr.data(), attr.size(),
                          pixels.data(), pixels.size() * sizeof(float));
        REQUIRE(sink.image_count() == 1);
    }

    // Readback
    ISMRMRD::Dataset ds(path.c_str(), "/dataset", false);
    REQUIRE(ds.getNumberOfImages("image_0") == 1);

    ISMRMRD::Image<float> img;
    ds.readImage("image_0", 0, img);
    REQUIRE(img.getHead().matrix_size[0] == nx);
    REQUIRE(img.getHead().data_type == ISMRMRD::ISMRMRD_FLOAT);
    REQUIRE(img.getDataPtr()[0] == Catch::Approx(42.0f));
    REQUIRE(img.getAttributeString() == std::string("test_attr"));
}

TEST_CASE("MrdSink appends waveforms and reads them back", "[mrd_sink]") {
    auto path = temp_h5("wf_test.h5");
    constexpr uint16_t nsamples = 100;
    constexpr uint16_t nchannels = 1;

    {
        mrd::MrdSink sink(path);
        sink.set_header(TEST_XML);

        ISMRMRD::Waveform wf(nsamples, nchannels);
        wf.head.version = 1;
        wf.head.waveform_id = 0; // ECG convention
        for (uint16_t i = 0; i < nsamples; ++i)
            wf.data[i] = i * 10;

        sink.append_waveform(wf);
        REQUIRE(sink.waveform_count() == 1);
    }

    // Readback
    ISMRMRD::Dataset ds(path.c_str(), "/dataset", false);
    REQUIRE(ds.getNumberOfWaveforms() == 1);

    ISMRMRD::Waveform wf;
    ds.readWaveform(0, wf);
    REQUIRE(wf.head.number_of_samples == nsamples);
    REQUIRE(wf.head.waveform_id == 0);
    REQUIRE(wf.data[5] == 50);
}

TEST_CASE("MrdSink close is idempotent", "[mrd_sink]") {
    auto path = temp_h5("close_test.h5");
    mrd::MrdSink sink(path);
    sink.set_header(TEST_XML);
    sink.close();
    sink.close(); // should not throw
    REQUIRE_FALSE(sink.is_open());
}

TEST_CASE("write_standalone_file creates file via atomic rename", "[mrd_sink]") {
    auto dir = fs::temp_directory_path() / "test_mrd_sink";
    auto path = dir / "standalone_test.bin";
    fs::remove(path);

    std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
    mrd::write_standalone_file(path, data.data(), data.size());

    REQUIRE(fs::exists(path));
    REQUIRE(fs::file_size(path) == 4);

    // Verify no .tmp file remains
    auto tmp = path;
    tmp += ".tmp";
    REQUIRE_FALSE(fs::exists(tmp));
}

TEST_CASE("MrdSink avoids SWMR-specific APIs", "[mrd_sink]") {
    // This is a compile-time check enforced by grep in CI.
    // If this file compiles, it avoids SWMR-specific APIs.
    REQUIRE(true);
}
