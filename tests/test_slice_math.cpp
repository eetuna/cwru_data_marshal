/*
 * tests/test_slice_math.cpp
 * Slice geometry math for the slice_agent command channel: Euler
 * round-trips against the agent's own buildRotMatrix convention, relative
 * moves in the slice frame, handedness, wire packing.
 */

#include <catch2/catch_all.hpp>

#include <cmath>
#include <random>

#include "slice_math.hpp"

using namespace slice_math;

namespace {

double max_abs_diff(const Mat3& a, const Mat3& b) {
    double m = 0.0;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            m = std::max(m, std::fabs(a[r][c] - b[r][c]));
    return m;
}

double vec_dist(const Vec3& a, const Vec3& b) {
    return norm({a[0]-b[0], a[1]-b[1], a[2]-b[2]});
}

} // namespace

TEST_CASE("Euler zyx round-trips through the agent's buildRotMatrix", "[slice][math]") {
    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> full(-kPi, kPi);
    std::uniform_real_distribution<double> half(-kPi / 2 + 1e-3, kPi / 2 - 1e-3);

    for (int i = 0; i < 5000; ++i) {
        const double rx = full(rng), ry = half(rng), rz = full(rng);
        const Mat3 R = rot_from_euler_zyx(rx, ry, rz);
        double ex, ey, ez;
        euler_zyx_from_rot(R, ex, ey, ez);
        const Mat3 R2 = rot_from_euler_zyx(ex, ey, ez);
        REQUIRE(max_abs_diff(R, R2) < 1e-9);
    }
}

TEST_CASE("Euler zyx handles gimbal lock at ry = +-90 deg", "[slice][math]") {
    for (double ry : {kPi / 2, -kPi / 2}) {
        const Mat3 R = rot_from_euler_zyx(0.3, ry, 0.7);
        double ex, ey, ez;
        euler_zyx_from_rot(R, ex, ey, ez);
        const Mat3 R2 = rot_from_euler_zyx(ex, ey, ez);
        CHECK(max_abs_diff(R, R2) < 1e-9);
    }
}

TEST_CASE("Identity geometry maps to zero command", "[slice][math]") {
    Geometry g;   // defaults: read=x, phase=y, slice=z, position 0
    const auto c = command_from_geometry(g);
    CHECK(c.tx == Catch::Approx(0.0));
    CHECK(c.rx == Catch::Approx(0.0).margin(1e-9));
    CHECK(c.ry == Catch::Approx(0.0).margin(1e-9));
    CHECK(c.rz == Catch::Approx(0.0).margin(1e-9));
    CHECK(c.flags == kFlagUpdate);
}

TEST_CASE("command_from_geometry reproduces the direction rows", "[slice][math]") {
    // Arbitrary right-handed frame from Euler angles, expressed as rows.
    const Mat3 R = rot_from_euler_zyx(0.4, -0.2, 0.9);
    const Geometry g = geometry_from_rot(R, {12.5, -3.0, 40.0});
    const auto c = command_from_geometry(g);
    CHECK(c.tx == Catch::Approx(12.5));
    CHECK(c.ty == Catch::Approx(-3.0));
    CHECK(c.tz == Catch::Approx(40.0));
    // The agent will rebuild R from these degrees:
    const Mat3 back = rot_from_euler_zyx(c.rx * kDeg2Rad, c.ry * kDeg2Rad, c.rz * kDeg2Rad);
    CHECK(max_abs_diff(R, back) < 1e-9);
}

TEST_CASE("Transpose option sends R^T", "[slice][math]") {
    const Mat3 R = rot_from_euler_zyx(0.4, -0.2, 0.9);
    const Geometry g = geometry_from_rot(R, {0, 0, 0});
    AxisOptions o; o.transpose = true;
    const auto c = command_from_geometry(g, o);
    const Mat3 back = rot_from_euler_zyx(c.rx * kDeg2Rad, c.ry * kDeg2Rad, c.rz * kDeg2Rad);
    CHECK(max_abs_diff(transpose(R), back) < 1e-9);
}

TEST_CASE("Axis sign and swap options", "[slice][math]") {
    Geometry g;
    g.position = {10, 20, 30};
    AxisOptions o;
    o.axis_sign = {1, -1, 1};
    const auto s = apply_axis_options(g, o);
    CHECK(s.position[1] == Catch::Approx(-20));
    CHECK(s.phase_dir[1] == Catch::Approx(-1));

    AxisOptions sw; sw.swap_read_phase = true;
    const auto w = apply_axis_options(g, sw);
    CHECK(w.read_dir[1] == Catch::Approx(1));
    CHECK(w.phase_dir[0] == Catch::Approx(1));
    CHECK(is_right_handed(w));   // normal recomputed as read x phase
}

TEST_CASE("Right-handedness check", "[slice][math]") {
    Geometry g;
    CHECK(is_right_handed(g));
    g.slice_dir = {0, 0, -1};
    CHECK_FALSE(is_right_handed(g));
}

TEST_CASE("apply_delta: +1 mm through-plane moves along the slice normal", "[slice][delta]") {
    // Tilted slice: normal is not a scanner axis.
    const Mat3 R = rot_from_euler_zyx(0.5, 0.3, -0.2);
    const Geometry base = geometry_from_rot(R, {5, 6, 7});
    const Geometry moved = apply_delta(base, {0, 0, 1}, {0, 0, 0});
    const Vec3 expected = add(base.position, base.slice_dir);
    CHECK(vec_dist(moved.position, expected) < 1e-9);
    // Orientation unchanged
    CHECK(vec_dist(moved.read_dir, base.read_dir) < 1e-9);
    CHECK(vec_dist(moved.slice_dir, base.slice_dir) < 1e-9);
}

TEST_CASE("apply_delta: in-plane translation follows read/phase", "[slice][delta]") {
    const Mat3 R = rot_from_euler_zyx(0.5, 0.3, -0.2);
    const Geometry base = geometry_from_rot(R, {0, 0, 0});
    const Geometry moved = apply_delta(base, {2, -3, 0}, {0, 0, 0});
    const Vec3 expected = add(scale(base.read_dir, 2), scale(base.phase_dir, -3));
    CHECK(vec_dist(moved.position, expected) < 1e-9);
}

TEST_CASE("apply_delta: rotation about read axis keeps read_dir fixed", "[slice][delta]") {
    const Mat3 R = rot_from_euler_zyx(0.5, 0.3, -0.2);
    const Geometry base = geometry_from_rot(R, {0, 0, 0});
    const double th = 10.0 * kDeg2Rad;
    const Geometry moved = apply_delta(base, {0, 0, 0}, {th, 0, 0});
    CHECK(vec_dist(moved.read_dir, base.read_dir) < 1e-9);
    CHECK(std::acos(std::clamp(dot(moved.slice_dir, base.slice_dir), -1.0, 1.0))
          == Catch::Approx(th).margin(1e-9));
    CHECK(is_right_handed(moved, 1e-9));
    // Position unchanged by a pure rotation
    CHECK(vec_dist(moved.position, base.position) < 1e-12);
}

TEST_CASE("apply_delta: rotation about the slice normal is in-plane", "[slice][delta]") {
    Geometry base;   // axial identity
    const double th = 90.0 * kDeg2Rad;
    const Geometry moved = apply_delta(base, {0, 0, 0}, {0, 0, th});
    CHECK(vec_dist(moved.slice_dir, base.slice_dir) < 1e-9);
    CHECK(vec_dist(moved.read_dir, {0, 1, 0}) < 1e-9);   // x rotated to y about z
    CHECK(vec_dist(moved.phase_dir, {-1, 0, 0}) < 1e-9);
}

TEST_CASE("apply_delta keeps the frame orthonormal over many steps", "[slice][delta]") {
    Geometry g;
    for (int i = 0; i < 2000; ++i) {
        g = apply_delta(g, {0.1, -0.05, 0.2}, {0.01, -0.02, 0.015});
    }
    CHECK(norm(g.read_dir) == Catch::Approx(1.0).margin(1e-9));
    CHECK(norm(g.phase_dir) == Catch::Approx(1.0).margin(1e-9));
    CHECK(norm(g.slice_dir) == Catch::Approx(1.0).margin(1e-9));
    CHECK(std::fabs(dot(g.read_dir, g.phase_dir)) < 1e-9);
    CHECK(is_right_handed(g, 1e-9));
}

TEST_CASE("Wire packing is 56 bytes little-endian and round-trips", "[slice][wire]") {
    WireCommand c;
    c.tx = 1.5; c.ty = -2.25; c.tz = 100.0;
    c.rx = 10.0; c.ry = -20.5; c.rz = 33.0;
    c.flags = kFlagQuit;
    const auto bytes = to_wire(c);
    REQUIRE(bytes.size() == 56);
    // flags at offset 48, little-endian 0x0000DEAD
    CHECK(bytes[48] == 0xAD);
    CHECK(bytes[49] == 0xDE);
    CHECK(bytes[50] == 0x00);
    CHECK(bytes[51] == 0x00);
    // tx = 1.5 as IEEE754 LE: 00 00 00 00 00 00 F8 3F
    CHECK(bytes[6] == 0xF8);
    CHECK(bytes[7] == 0x3F);
    const auto back = from_wire(bytes.data());
    CHECK(back.tx == c.tx);
    CHECK(back.ty == c.ty);
    CHECK(back.tz == c.tz);
    CHECK(back.rx == c.rx);
    CHECK(back.ry == c.ry);
    CHECK(back.rz == c.rz);
    CHECK(back.flags == kFlagQuit);
}
