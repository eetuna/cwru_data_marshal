/*
 * File: src/slice_math.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Slice-command state for the scanner-side slice_agent channel.
 *
 * The reference client is Andrew's slice_control.cpp
 * (dynamic-slice-position-main/agent/slice_control.cpp). It keeps six
 * absolute numbers — tx ty tz (mm) and rx ry rz (degrees) — starting at
 * zero, and on every key:
 *   arrows / PgUp / PgDn : (tx,ty,tz) += step * row k of buildRotMatrix(rx,ry,rz)
 *                          (row 0 = readout, row 1 = phase, row 2 = slice normal)
 *   W/S, A/D, Q/E        : rx / ry / rz += step
 * and sends the six totals as a 56-byte SliceCommand. SliceState/apply_step
 * below are that arithmetic verbatim, so the marshal puts the same bytes on
 * the wire as his tool would for the same moves. buildRotMatrix is ported
 * verbatim too; how the sequence interprets its output is then identical
 * to Andrew's own sessions.
 *
 * Header-only, no external dependencies.
 */

#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace slice_math {

using Vec3 = std::array<double, 3>;
using Mat3 = std::array<Vec3, 3>;   // Mat3[row][col]

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg2Rad = kPi / 180.0;
constexpr double kRad2Deg = 180.0 / kPi;

// ---- basic vector / matrix helpers -----------------------------------------

inline double dot(const Vec3& a, const Vec3& b) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1]*b[2] - a[2]*b[1],
            a[2]*b[0] - a[0]*b[2],
            a[0]*b[1] - a[1]*b[0]};
}

inline double norm(const Vec3& a) { return std::sqrt(dot(a, a)); }

inline Vec3 normalized(const Vec3& a) {
    const double n = norm(a);
    if (n <= 0.0) return a;
    return {a[0]/n, a[1]/n, a[2]/n};
}

inline Vec3 add(const Vec3& a, const Vec3& b) {
    return {a[0]+b[0], a[1]+b[1], a[2]+b[2]};
}

inline Vec3 scale(const Vec3& a, double s) {
    return {a[0]*s, a[1]*s, a[2]*s};
}

inline Mat3 mul(const Mat3& a, const Mat3& b) {
    Mat3 out{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            double s = 0.0;
            for (int k = 0; k < 3; ++k) s += a[r][k] * b[k][c];
            out[r][c] = s;
        }
    return out;
}

// Verbatim port of slice_agent.cpp / slice_control.cpp buildRotMatrix():
// R = Rz * Ry * Rx, angles in radians.
inline Mat3 build_rot_matrix(double rx, double ry, double rz) {
    const double cx = std::cos(rx), sx = std::sin(rx);
    const double cy = std::cos(ry), sy = std::sin(ry);
    const double cz = std::cos(rz), sz = std::sin(rz);
    const Mat3 Rx = {{{1, 0, 0}, {0, cx, -sx}, {0, sx, cx}}};
    const Mat3 Ry = {{{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}}};
    const Mat3 Rz = {{{cz, -sz, 0}, {sz, cz, 0}, {0, 0, 1}}};
    return mul(Rz, mul(Ry, Rx));
}

// Inverse of build_rot_matrix (used only by slice_target, which arrives as
// direction vectors). For R = Rz*Ry*Rx:
//   R[2][0] = -sin(ry)
//   R[2][1] = cos(ry) sin(rx),  R[2][2] = cos(ry) cos(rx)
//   R[1][0] = sin(rz) cos(ry),  R[0][0] = cos(rz) cos(ry)
// Gimbal lock (|ry| = 90 deg): rx and rz are not separable; choose rx = 0.
inline void euler_from_rot_matrix(const Mat3& R, double& rx, double& ry, double& rz) {
    double s = -R[2][0];
    if (s > 1.0) s = 1.0;
    if (s < -1.0) s = -1.0;
    ry = std::asin(s);
    if (std::fabs(std::cos(ry)) > 1e-6) {
        rx = std::atan2(R[2][1], R[2][2]);
        rz = std::atan2(R[1][0], R[0][0]);
    } else {
        rx = 0.0;
        rz = std::atan2(-R[0][1], R[1][1]);
    }
}

// ---- the six numbers ---------------------------------------------------------

// slice_control.cpp:421-422 — all zero at start ("identity"), i.e. exactly
// what slice_agent publishes on connect.
struct SliceState {
    Vec3 t{0, 0, 0};       // tx ty tz, mm
    Vec3 r_deg{0, 0, 0};   // rx ry rz, degrees
};

inline Mat3 rot_of(const SliceState& s) {
    return build_rot_matrix(s.r_deg[0] * kDeg2Rad, s.r_deg[1] * kDeg2Rad, s.r_deg[2] * kDeg2Rad);
}

// One relative move, slice_control.cpp:472-490 verbatim:
//   translation_mm[k] steps along row k of the CURRENT rotation matrix
//   (k = 0 readout, 1 phase, 2 slice normal — arrows / PgUp / PgDn),
//   rotation_deg[k] is added to the k-th Euler angle (W/S, A/D, Q/E).
// A key press in his tool is one or the other; when both are given here,
// translation is applied first (with the pre-rotation matrix), then the
// angles — the same as pressing the translation key and then the rotation key.
inline void apply_step(SliceState& s, const Vec3& translation_mm, const Vec3& rotation_deg) {
    const Mat3 R = rot_of(s);
    for (int k = 0; k < 3; ++k) {
        if (translation_mm[k] == 0.0) continue;
        for (int i = 0; i < 3; ++i) s.t[i] += translation_mm[k] * R[k][i];
    }
    for (int k = 0; k < 3; ++k) s.r_deg[k] += rotation_deg[k];
}

// ---- absolute geometry (slice_target only) ---------------------------------

// Absolute slice geometry as direction vectors (ISMRMRD image-header style).
struct Geometry {
    Vec3 position{0, 0, 0};
    Vec3 read_dir{1, 0, 0};
    Vec3 phase_dir{0, 1, 0};
    Vec3 slice_dir{0, 0, 1};
};

// Gram-Schmidt on (read, phase, read x phase): cleans float rounding.
inline Mat3 orthonormalize_rows(const Mat3& m) {
    Vec3 r0 = normalized(m[0]);
    Vec3 r1 = add(m[1], scale(r0, -dot(m[1], r0)));
    r1 = normalized(r1);
    Vec3 r2 = cross(r0, r1);
    return {r0, r1, r2};
}

// read x phase must point along slice (not against it); a left-handed frame
// has no rotation-matrix (hence no Euler) representation. A sign test:
// unit length / orthogonality are checked separately by the caller, and a
// slightly de-normalised float32 header must not be misreported as
// left-handed.
inline bool is_right_handed(const Geometry& g) {
    return dot(cross(g.read_dir, g.phase_dir), g.slice_dir) > 0.5;
}

// Direction vectors -> the six numbers. The vectors are taken as the rows of
// buildRotMatrix (row 0 = read, 1 = phase, 2 = normal), which is how
// slice_control's own translation keys read that matrix; the angles are the
// Euler triple that rebuilds those rows. Caller checks is_right_handed first.
inline SliceState state_from_geometry(const Geometry& g) {
    const Mat3 rows = orthonormalize_rows({g.read_dir, g.phase_dir, g.slice_dir});
    double rx, ry, rz;
    euler_from_rot_matrix(rows, rx, ry, rz);
    SliceState s;
    s.t = g.position;
    s.r_deg = {rx * kRad2Deg, ry * kRad2Deg, rz * kRad2Deg};
    return s;
}

// The six numbers -> direction vectors (rows of buildRotMatrix), for display.
inline Geometry geometry_from_state(const SliceState& s) {
    const Mat3 R = rot_of(s);
    Geometry g;
    g.position = s.t;
    g.read_dir = R[0];
    g.phase_dir = R[1];
    g.slice_dir = R[2];
    return g;
}

// ---- wire command ------------------------------------------------------------

// 56-byte SliceCommand as defined in agent/slice_net.h (little-endian).
struct WireCommand {
    double tx{0}, ty{0}, tz{0};   // mm, PCS
    double rx{0}, ry{0}, rz{0};   // degrees
    uint32_t flags{0};            // 0 = update, 0xDEAD = quit
    uint32_t pad{0};
};
static_assert(sizeof(WireCommand) == 56, "SliceCommand must be 56 bytes");

constexpr uint32_t kFlagUpdate = 0;
constexpr uint32_t kFlagQuit   = 0xDEAD;
constexpr size_t   kWireBytes  = 56;

inline WireCommand command_from_state(const SliceState& s) {
    WireCommand c;
    c.tx = s.t[0] + 0.0;   // "+ 0.0" folds -0.0 into 0.0 for tidy JSON/logs
    c.ty = s.t[1] + 0.0;
    c.tz = s.t[2] + 0.0;
    c.rx = s.r_deg[0] + 0.0;
    c.ry = s.r_deg[1] + 0.0;
    c.rz = s.r_deg[2] + 0.0;
    c.flags = kFlagUpdate;
    return c;
}

// Serialize little-endian regardless of host order.
inline void write_le64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}
inline void write_le32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}
inline uint64_t read_le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}
inline uint32_t read_le32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}

inline std::array<uint8_t, kWireBytes> to_wire(const WireCommand& c) {
    std::array<uint8_t, kWireBytes> out{};
    const double d[6] = {c.tx, c.ty, c.tz, c.rx, c.ry, c.rz};
    for (int i = 0; i < 6; ++i) {
        uint64_t bits;
        std::memcpy(&bits, &d[i], sizeof(bits));
        write_le64(out.data() + 8 * i, bits);
    }
    write_le32(out.data() + 48, c.flags);
    write_le32(out.data() + 52, c.pad);
    return out;
}

inline WireCommand from_wire(const uint8_t* p) {
    WireCommand c;
    double* d[6] = {&c.tx, &c.ty, &c.tz, &c.rx, &c.ry, &c.rz};
    for (int i = 0; i < 6; ++i) {
        const uint64_t bits = read_le64(p + 8 * i);
        std::memcpy(d[i], &bits, sizeof(bits));
    }
    c.flags = read_le32(p + 48);
    c.pad = read_le32(p + 52);
    return c;
}

} // namespace slice_math
