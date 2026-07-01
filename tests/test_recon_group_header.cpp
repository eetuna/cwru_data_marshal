/*
 * tests/test_recon_group_header.cpp
 * Regression test for HIGH #9 in MRI_MARSHAL_BUGS_FINAL_2026-04-18.md:
 * recon_group_is_complete() used to treat recon_expected_slices == 0 (the
 * default before the XML header is parsed) as "complete", so the first
 * recon image arriving before the header would be published as a full
 * multislice stack.
 *
 * Post-fix: the function also checks state.header_received and returns
 * false until the header has been parsed.
 */

#include <catch2/catch_all.hpp>

#include "marshal_state.hpp"
#include "live_image_store.hpp"

TEST_CASE("recon_group_is_complete waits for header", "[live_image_store][header]") {
    MarshalState state;
    ReconLatestGroupState group;

    SECTION("Without header: never complete, even with slices") {
        REQUIRE_FALSE(state.header_received.load());
        REQUIRE_FALSE(mrd::recon_group_is_complete(state, group));

        group.seen_slices.push_back(0);
        REQUIRE_FALSE(mrd::recon_group_is_complete(state, group));

        // expected_slices already 0 by default — old code would have said true.
        state.recon_expected_slices = 0;
        REQUIRE_FALSE(mrd::recon_group_is_complete(state, group));
    }

    SECTION("With header + single-slice scan: complete after any slice") {
        state.header_received.store(true);
        state.recon_expected_slices = 1;
        REQUIRE(mrd::recon_group_is_complete(state, group));
    }

    SECTION("With header + multislice: incomplete until all slices seen") {
        state.header_received.store(true);
        state.recon_expected_slices = 5;

        REQUIRE_FALSE(mrd::recon_group_is_complete(state, group));

        group.seen_slices.push_back(0);
        group.seen_slices.push_back(1);
        group.seen_slices.push_back(2);
        REQUIRE_FALSE(mrd::recon_group_is_complete(state, group));

        group.seen_slices.push_back(3);
        group.seen_slices.push_back(4);
        REQUIRE(mrd::recon_group_is_complete(state, group));
    }

    SECTION("Header cleared after scan close: incomplete again") {
        state.header_received.store(true);
        state.recon_expected_slices = 1;
        REQUIRE(mrd::recon_group_is_complete(state, group));

        state.header_received.store(false);
        REQUIRE_FALSE(mrd::recon_group_is_complete(state, group));
    }
}
