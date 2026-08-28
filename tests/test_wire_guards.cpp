/*
 * tests/test_wire_guards.cpp
 * Unit tests for wire_guards.hpp — checked arithmetic + sanity caps that
 * fix HIGH #5, #6, #7 in MRI_MARSHAL_BUGS_FINAL_2026-04-18.md.
 *
 * These validate the guard helpers directly. Integration against the actual
 * parse paths is exercised by the existing it_http + test_mrd_sink suites
 * and would require a full MRD socket harness to reach the parse sites.
 */

#include <catch2/catch_all.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "wire_guards.hpp"

using namespace mrd;

TEST_CASE("checked_add detects overflow", "[wire_guards]") {
    std::size_t out = 0;
    REQUIRE(checked_add(100, 200, out));
    REQUIRE(out == 300);

    REQUIRE(checked_add(0, 0, out));
    REQUIRE(out == 0);

    // Overflow
    const std::size_t big = std::numeric_limits<std::size_t>::max() - 10;
    REQUIRE_FALSE(checked_add(big, 20, out));
    REQUIRE(checked_add(big, 10, out));
    REQUIRE(out == std::numeric_limits<std::size_t>::max());
}

TEST_CASE("checked_mul detects overflow", "[wire_guards]") {
    std::size_t out = 0;
    REQUIRE(checked_mul(0, 12345, out));
    REQUIRE(out == 0);
    REQUIRE(checked_mul(12345, 0, out));
    REQUIRE(out == 0);

    REQUIRE(checked_mul(123, 456, out));
    REQUIRE(out == 56088);

    // Demonstrate overflow on 64-bit size_t. (u32max * u32max just barely
    // fits, so pick clearly-overflowing values.)
    const std::size_t half_max = (std::numeric_limits<std::size_t>::max() / 2) + 1;
    REQUIRE_FALSE(checked_mul(half_max, 3, out));

    // u16max^4 × 16 (the audit attack: matrix^3 × channels × dtype) overflows.
    const std::size_t u16max = 65535;
    std::size_t prod = 1;
    REQUIRE(checked_mul(prod, u16max, prod));  // 65535
    REQUIRE(checked_mul(prod, u16max, prod));  // 4.29e9
    REQUIRE(checked_mul(prod, u16max, prod));  // 2.81e14
    REQUIRE(checked_mul(prod, u16max, prod));  // 1.84e19 — just fits
    REQUIRE_FALSE(checked_mul(prod, 16, out)); // × 16 overflows
}

TEST_CASE("compute_pixel_bytes rejects adversarial dimensions", "[wire_guards]") {
    std::size_t out = 0;

    // Normal case
    REQUIRE(compute_pixel_bytes(128, 128, 1, 1, 4, out));
    REQUIRE(out == 65536);

    // Zero-width rejected (not valid MR image).
    REQUIRE_FALSE(compute_pixel_bytes(0, 128, 1, 1, 4, out));
    REQUIRE_FALSE(compute_pixel_bytes(128, 0, 1, 1, 4, out));

    // Zero depth/channels treated as 1 (matches existing std::max<uint16_t>(..., 1)).
    REQUIRE(compute_pixel_bytes(64, 64, 0, 0, 4, out));
    REQUIRE(out == 64 * 64 * 4);

    // Per-dim cap
    REQUIRE_FALSE(compute_pixel_bytes(65535, 65535, 65535, 65535, 16, out));
    REQUIRE_FALSE(compute_pixel_bytes(kMaxImageDim + 1, 128, 1, 1, 4, out));
    REQUIRE_FALSE(compute_pixel_bytes(128, 128, 1, kMaxImageChannels + 1, 4, out));

    // Bad datatype size
    REQUIRE_FALSE(compute_pixel_bytes(128, 128, 1, 1, 0, out));
    REQUIRE_FALSE(compute_pixel_bytes(128, 128, 1, 1, 32, out));
}

TEST_CASE("validate_attr_len rejects adversarial sizes", "[wire_guards]") {
    std::size_t out = 0;

    REQUIRE(validate_attr_len(0, out));
    REQUIRE(out == 0);

    REQUIRE(validate_attr_len(1024, out));
    REQUIRE(out == 1024);

    REQUIRE_FALSE(validate_attr_len(kMaxAttrBytes + 1, out));
    REQUIRE_FALSE(validate_attr_len(std::numeric_limits<std::uint64_t>::max(), out));
}

TEST_CASE("compute_image_body_total rejects the OOB-write attack from audit #5/#6",
          "[wire_guards][security]") {
    std::size_t out = 0;

    // Happy path
    REQUIRE(compute_image_body_total(340, 1024, 65536, out));
    REQUIRE(out == 340 + 8 + 1024 + 65536);

    // The specific attack: attr_len near SIZE_MAX makes
    // header+8+attr_len+pixels wrap small. Must reject.
    const std::size_t evil = std::numeric_limits<std::size_t>::max() - 100;
    REQUIRE_FALSE(compute_image_body_total(340, evil, 65536, out));

    // Under normal caps the attr_len would have been rejected first, but the
    // overflow check is belt-and-suspenders. Aggregate > kMaxImageFrameBytes
    // also rejected.
    REQUIRE_FALSE(compute_image_body_total(340, kMaxAttrBytes,
                                            kMaxImageFrameBytes, out));
}

TEST_CASE("validate_len_prefix_body rejects oversized frames (audit #12)",
          "[wire_guards]") {
    std::size_t out = 0;
    REQUIRE(validate_len_prefix_body(1024, out));
    REQUIRE(out == 1028);

    REQUIRE_FALSE(validate_len_prefix_body(
        static_cast<std::uint32_t>(kMaxLenPrefixBodyBytes + 1), out));
    // 4 GiB claim: rejected by cap (well before 4+len overflows).
    REQUIRE_FALSE(validate_len_prefix_body(
        std::numeric_limits<std::uint32_t>::max(), out));
}

TEST_CASE("acquisition/waveform payload sizes are capped (audit 2026-08-28 #4)",
          "[wire_guards]") {
    std::size_t traj = 0, samp = 0, wf = 0;

    // Realistic: 2D traj, 256 samples, 32 channels.
    REQUIRE(mrd::compute_acquisition_payload_bytes(2, 256, 32, traj, samp));
    CHECK(traj == 2u * 256u * 4u);
    CHECK(samp == 256u * 32u * 8u);

    // Max uint16 fields: ~34 GiB — must be rejected, not allocated.
    CHECK_FALSE(mrd::compute_acquisition_payload_bytes(65535, 65535, 65535, traj, samp));
    // Traj alone over the cap.
    CHECK_FALSE(mrd::compute_acquisition_payload_bytes(65535, 65535, 0, traj, samp));
    // Exactly at the cap is allowed; one byte over is not.
    const std::size_t cap_samples = mrd::kMaxImageFrameBytes / 8;
    REQUIRE(mrd::compute_acquisition_payload_bytes(0, cap_samples, 1, traj, samp));
    CHECK(samp == mrd::kMaxImageFrameBytes);
    CHECK_FALSE(mrd::compute_acquisition_payload_bytes(0, cap_samples + 1, 1, traj, samp));

    // Waveform: realistic ECG.
    REQUIRE(mrd::compute_waveform_data_bytes(1000, 4, wf));
    CHECK(wf == 1000u * 4u * 4u);
    // Max uint16 fields: ~16 GiB.
    CHECK_FALSE(mrd::compute_waveform_data_bytes(65535, 65535, wf));
    // Overflow of the product itself.
    CHECK_FALSE(mrd::compute_waveform_data_bytes(
        std::numeric_limits<std::size_t>::max(), 2, wf));
}
