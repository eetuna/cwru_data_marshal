/*
 * tests/test_slice_math.cpp
 * The marshal's six-number slice state must reproduce Andrew's
 * slice_control.cpp byte-for-byte. A verbatim port of that tool's state
 * machine lives below and is driven with random key sequences against
 * slice_math::apply_step. Plus: Euler <-> matrix for slice_target, wire
 * packing.
 */

#include <catch2/catch_all.hpp>

#include <cmath>
#include <random>

#include "slice_math.hpp"

using namespace slice_math;

namespace {

// ---- verbatim port of slice_control.cpp:108-131 + 421-500 ------------------
void buildRotMatrix(double out[3][3], double ax, double ay, double az) {
    const double cx = std::cos(ax), sx = std::sin(ax);
    const double cy = std::cos(ay), sy = std::sin(ay);
    const double cz = std::cos(az), sz = std::sin(az);
    double Rx[3][3] = {{1,0,0},{0,cx,-sx},{0,sx,cx}};
    double Ry[3][3] = {{cy,0,sy},{0,1,0},{-sy,0,cy}};
    double Rz[3][3] = {{cz,-sz,0},{sz,cz,0},{0,0,1}};
    double tmp[3][3] = {};
    for (int r=0;r<3;++r) for (int c=0;c<3;++c) for (int k=0;k<3;++k) tmp[r][c] += Ry[r][k]*Rx[k][c];
    for (int r=0;r<3;++r) for (int c=0;c<3;++c) { out[r][c]=0.0; for (int k=0;k<3;++k) out[r][c] += Rz[r][k]*tmp[k][c]; }
}
constexpr double kD2R = M_PI / 180.0;

struct SliceControl {
    double tx = 0.0, ty = 0.0, tz = 0.0;
    double rx = 0.0, ry = 0.0, rz = 0.0;
    double tStep = 1.0, rStep = 1.0;
    enum Key { RIGHT, LEFT, UP, DOWN, PGUP, PGDN, W, S, D, A, E, Q, ZERO };
    void press(Key k) {
        double curRot[3][3];
        buildRotMatrix(curRot, rx * kD2R, ry * kD2R, rz * kD2R);
        switch (k) {
            case RIGHT: tx += tStep*curRot[0][0]; ty += tStep*curRot[0][1]; tz += tStep*curRot[0][2]; break;
            case LEFT:  tx -= tStep*curRot[0][0]; ty -= tStep*curRot[0][1]; tz -= tStep*curRot[0][2]; break;
            case UP:    tx += tStep*curRot[1][0]; ty += tStep*curRot[1][1]; tz += tStep*curRot[1][2]; break;
            case DOWN:  tx -= tStep*curRot[1][0]; ty -= tStep*curRot[1][1]; tz -= tStep*curRot[1][2]; break;
            case PGUP:  tx += tStep*curRot[2][0]; ty += tStep*curRot[2][1]; tz += tStep*curRot[2][2]; break;
            case PGDN:  tx -= tStep*curRot[2][0]; ty -= tStep*curRot[2][1]; tz -= tStep*curRot[2][2]; break;
            case W: rx += rStep; break;
            case S: rx -= rStep; break;
            case D: ry += rStep; break;
            case A: ry -= rStep; break;
            case E: rz += rStep; break;
            case Q: rz -= rStep; break;
            case ZERO: tx = ty = tz = 0.0; rx = ry = rz = 0.0; break;
        }
    }
};

bool same(const SliceControl& sc, const SliceState& s, double tol = 1e-9) {
    return std::fabs(sc.tx - s.t[0]) < tol && std::fabs(sc.ty - s.t[1]) < tol && std::fabs(sc.tz - s.t[2]) < tol
        && std::fabs(sc.rx - s.r_deg[0]) < tol && std::fabs(sc.ry - s.r_deg[1]) < tol && std::fabs(sc.rz - s.r_deg[2]) < tol;
}

double max_abs_diff(const Mat3& a, const Mat3& b) {
    double m = 0.0;
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) m = std::max(m, std::fabs(a[r][c] - b[r][c]));
    return m;
}

} // namespace

TEST_CASE("build_rot_matrix is slice_control's buildRotMatrix", "[slice][math]") {
    std::mt19937 rng(1);
    std::uniform_real_distribution<double> ang(-M_PI, M_PI);
    for (int i = 0; i < 2000; ++i) {
        const double ax = ang(rng), ay = ang(rng), az = ang(rng);
        double ref[3][3]; buildRotMatrix(ref, ax, ay, az);
        const Mat3 R = build_rot_matrix(ax, ay, az);
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c)
            REQUIRE(R[r][c] == Catch::Approx(ref[r][c]).margin(1e-12));
    }
}

TEST_CASE("apply_step reproduces slice_control key by key", "[slice][state]") {
    SliceControl sc; SliceState s;

    // PgUp at identity: +1 mm along z
    sc.press(SliceControl::PGUP); apply_step(s, {0, 0, 1}, {0, 0, 0});
    CHECK(same(sc, s)); CHECK(s.t[2] == Catch::Approx(1.0));

    // W: tilt +10 about x — the angle is simply added (no sign games)
    sc.rStep = 10; sc.press(SliceControl::W); apply_step(s, {0, 0, 0}, {10, 0, 0});
    CHECK(same(sc, s)); CHECK(s.r_deg[0] == Catch::Approx(10.0));

    // PgUp after the tilt: along row 2 of the tilted matrix
    sc.press(SliceControl::PGUP); apply_step(s, {0, 0, 1}, {0, 0, 0});
    CHECK(same(sc, s));
    CHECK(s.t[1] == Catch::Approx(std::sin(10 * kD2R)).margin(1e-12));   // row2 = (0, sin, cos)

    // E: roll +30 about z, then D: pan +5 about y
    sc.rStep = 30; sc.press(SliceControl::E); apply_step(s, {0, 0, 0}, {0, 0, 30});
    sc.rStep = 5;  sc.press(SliceControl::D); apply_step(s, {0, 0, 0}, {0, 5, 0});
    CHECK(same(sc, s));

    // arrows: in-plane along rows 0 / 1
    sc.press(SliceControl::RIGHT); apply_step(s, {1, 0, 0}, {0, 0, 0});
    sc.press(SliceControl::DOWN);  apply_step(s, {0, -1, 0}, {0, 0, 0});
    CHECK(same(sc, s));

    // 0: reset
    sc.press(SliceControl::ZERO); s = SliceState{};
    CHECK(same(sc, s));
}

TEST_CASE("apply_step == slice_control over random key sequences", "[slice][state]") {
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> key(0, 12);
    std::uniform_int_distribution<int> stepi(0, 4);
    const double steps[5] = {0.1, 1.0, 2.0, 10.0, 30.0};
    for (int trial = 0; trial < 3000; ++trial) {
        SliceControl sc; SliceState s;
        const int n = 1 + trial % 40;
        for (int i = 0; i < n; ++i) {
            const int k = key(rng);
            const double st = steps[stepi(rng)];
            sc.tStep = st; sc.rStep = st;
            sc.press(static_cast<SliceControl::Key>(k));
            switch (k) {
                case SliceControl::RIGHT: apply_step(s, { st, 0, 0}, {0, 0, 0}); break;
                case SliceControl::LEFT:  apply_step(s, {-st, 0, 0}, {0, 0, 0}); break;
                case SliceControl::UP:    apply_step(s, {0,  st, 0}, {0, 0, 0}); break;
                case SliceControl::DOWN:  apply_step(s, {0, -st, 0}, {0, 0, 0}); break;
                case SliceControl::PGUP:  apply_step(s, {0, 0,  st}, {0, 0, 0}); break;
                case SliceControl::PGDN:  apply_step(s, {0, 0, -st}, {0, 0, 0}); break;
                case SliceControl::W: apply_step(s, {0, 0, 0}, { st, 0, 0}); break;
                case SliceControl::S: apply_step(s, {0, 0, 0}, {-st, 0, 0}); break;
                case SliceControl::D: apply_step(s, {0, 0, 0}, {0,  st, 0}); break;
                case SliceControl::A: apply_step(s, {0, 0, 0}, {0, -st, 0}); break;
                case SliceControl::E: apply_step(s, {0, 0, 0}, {0, 0,  st}); break;
                case SliceControl::Q: apply_step(s, {0, 0, 0}, {0, 0, -st}); break;
                case SliceControl::ZERO: s = SliceState{}; break;
            }
        }
        REQUIRE(same(sc, s, 1e-8));
    }
}

TEST_CASE("Wire command carries the six numbers unchanged", "[slice][wire]") {
    SliceState s; s.t = {1.5, -2.25, 100.0}; s.r_deg = {10.0, -20.5, 33.0};
    const auto c = command_from_state(s);
    CHECK(c.tx == 1.5); CHECK(c.ty == -2.25); CHECK(c.tz == 100.0);
    CHECK(c.rx == 10.0); CHECK(c.ry == -20.5); CHECK(c.rz == 33.0);
    CHECK(c.flags == kFlagUpdate);
    const auto bytes = to_wire(c);
    REQUIRE(bytes.size() == 56);
    CHECK(bytes[6] == 0xF8); CHECK(bytes[7] == 0x3F);   // 1.5 as IEEE754 LE
    const auto back = from_wire(bytes.data());
    CHECK(back.tx == c.tx); CHECK(back.rz == c.rz); CHECK(back.flags == c.flags);
    WireCommand q; q.flags = kFlagQuit;
    const auto qb = to_wire(q);
    CHECK(qb[48] == 0xAD); CHECK(qb[49] == 0xDE); CHECK(qb[50] == 0); CHECK(qb[51] == 0);
}

TEST_CASE("slice_target: direction rows -> angles -> same rows", "[slice][target]") {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> full(-M_PI, M_PI);
    std::uniform_real_distribution<double> half(-M_PI / 2 + 1e-3, M_PI / 2 - 1e-3);
    for (int i = 0; i < 2000; ++i) {
        const Mat3 R = build_rot_matrix(full(rng), half(rng), full(rng));
        Geometry g; g.position = {1, 2, 3}; g.read_dir = R[0]; g.phase_dir = R[1]; g.slice_dir = R[2];
        REQUIRE(is_right_handed(g, 1e-9));
        const SliceState s = state_from_geometry(g);
        CHECK(s.t[0] == 1.0);
        CHECK(max_abs_diff(rot_of(s), R) < 1e-9);
        const Geometry back = geometry_from_state(s);
        for (int k = 0; k < 3; ++k) {
            CHECK(back.read_dir[k]  == Catch::Approx(g.read_dir[k]).margin(1e-9));
            CHECK(back.slice_dir[k] == Catch::Approx(g.slice_dir[k]).margin(1e-9));
        }
    }
    // gimbal lock still round-trips the matrix
    for (double ry : {M_PI / 2, -M_PI / 2}) {
        const Mat3 R = build_rot_matrix(0.3, ry, 0.7);
        Geometry g; g.read_dir = R[0]; g.phase_dir = R[1]; g.slice_dir = R[2];
        CHECK(max_abs_diff(rot_of(state_from_geometry(g)), R) < 1e-9);
    }
}

TEST_CASE("Right-handedness check", "[slice][target]") {
    Geometry g;
    CHECK(is_right_handed(g));
    g.slice_dir = {0, 0, -1};
    CHECK_FALSE(is_right_handed(g));
}
