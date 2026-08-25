/*
 * File: src/slice_math.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Slice-geometry math for the slice_agent command channel.
 *
 * The scanner-side agent (dynamic-slice-position-main/agent/slice_agent.cpp)
 * accepts an ABSOLUTE slice geometry as three Euler angles (degrees) and a
 * slice center (mm, scanner PCS). It rebuilds the rotation as
 *     R = Rz(rz) * Ry(ry) * Rx(rx)
 * and the sequence uses the rows of R as the readout / phase / slice-normal
 * directions. This header converts between the marshal's direction-vector
 * representation (as found in ISMRMRD image headers) and that wire form,
 * and applies relative moves expressed in the slice's own frame.
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

// Absolute slice geometry: center in mm (scanner PCS) and three unit
// direction vectors (readout, phase-encode, slice normal).
struct Geometry {
    Vec3 position{0, 0, 0};
    Vec3 read_dir{1, 0, 0};
    Vec3 phase_dir{0, 1, 0};
    Vec3 slice_dir{0, 0, 1};
};

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

inline Mat3 identity() {
    return {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
}

inline Mat3 transpose(const Mat3& m) {
    Mat3 t;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            t[r][c] = m[c][r];
    return t;
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

inline double det3(const Mat3& m) {
    return m[0][0] * (m[1][1]*m[2][2] - m[1][2]*m[2][1])
         - m[0][1] * (m[1][0]*m[2][2] - m[1][2]*m[2][0])
         + m[0][2] * (m[1][0]*m[2][1] - m[1][1]*m[2][0]);
}

// Rotation matrix whose ROWS are the three direction vectors (the layout
// SliceXfm.h / applySliceTransform() use: row 0 = read, 1 = phase, 2 = normal).
inline Mat3 rot_from_geometry(const Geometry& g) {
    return {g.read_dir, g.phase_dir, g.slice_dir};
}

inline Geometry geometry_from_rot(const Mat3& rows, const Vec3& position) {
    Geometry g;
    g.position = position;
    g.read_dir = rows[0];
    g.phase_dir = rows[1];
    g.slice_dir = rows[2];
    return g;
}

// Gram-Schmidt on the rows: keeps read, makes phase orthogonal to read,
// derives the normal as read x phase (right-handed). Cleans up float
// rounding from image headers before Euler decomposition.
inline Mat3 orthonormalize_rows(const Mat3& m) {
    Vec3 r0 = normalized(m[0]);
    Vec3 r1 = add(m[1], scale(r0, -dot(m[1], r0)));
    r1 = normalized(r1);
    Vec3 r2 = cross(r0, r1);
    return {r0, r1, r2};
}

// Is the (read, phase, slice) frame right-handed? read x phase must point
// along slice. Left-handed frames cannot be produced by any Euler triple.
inline bool is_right_handed(const Geometry& g, double tol = 1e-3) {
    return dot(cross(g.read_dir, g.phase_dir), g.slice_dir) > 1.0 - tol;
}

// ---- Euler <-> matrix, in the agent's convention ---------------------------

// Verbatim port of slice_agent.cpp buildRotMatrix(): R = Rz * Ry * Rx, angles
// in radians.
inline Mat3 rot_from_euler_zyx(double rx, double ry, double rz) {
    const double cx = std::cos(rx), sx = std::sin(rx);
    const double cy = std::cos(ry), sy = std::sin(ry);
    const double cz = std::cos(rz), sz = std::sin(rz);
    const Mat3 Rx = {{{1, 0, 0}, {0, cx, -sx}, {0, sx, cx}}};
    const Mat3 Ry = {{{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}}};
    const Mat3 Rz = {{{cz, -sz, 0}, {sz, cz, 0}, {0, 0, 1}}};
    return mul(Rz, mul(Ry, Rx));
}

// Inverse of rot_from_euler_zyx. For R = Rz*Ry*Rx:
//   R[2][0] = -sin(ry)
//   R[2][1] = cos(ry) sin(rx),  R[2][2] = cos(ry) cos(rx)
//   R[1][0] = sin(rz) cos(ry),  R[0][0] = cos(rz) cos(ry)
// Gimbal lock (|ry| = 90 deg): rx and rz are not separable; choose rx = 0.
inline void euler_zyx_from_rot(const Mat3& R, double& rx, double& ry, double& rz) {
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

// ---- relative moves in the slice frame -------------------------------------

// Rotate vector v about unit axis k by angle theta (Rodrigues).
inline Vec3 rotate_about(const Vec3& v, const Vec3& k, double theta) {
    const double c = std::cos(theta), s = std::sin(theta);
    const Vec3 kxv = cross(k, v);
    const double kdv = dot(k, v);
    return {v[0]*c + kxv[0]*s + k[0]*kdv*(1 - c),
            v[1]*c + kxv[1]*s + k[1]*kdv*(1 - c),
            v[2]*c + kxv[2]*s + k[2]*kdv*(1 - c)};
}

// Apply a relative move expressed in the slice's own frame:
//   translation_mm = [along read, along phase, along slice normal]
//   rotation_rad   = [about read, about phase, about slice normal]
// Rotations are applied in that order, each about the axis as it stands
// after the previous rotation. This matches what the UI buttons mean:
// "+1" = one mm through-plane, "tilt X" = tilt about the readout axis.
inline Geometry apply_delta(const Geometry& base,
                            const Vec3& translation_mm,
                            const Vec3& rotation_rad) {
    Geometry g = base;

    g.position = add(g.position, scale(g.read_dir,  translation_mm[0]));
    g.position = add(g.position, scale(g.phase_dir, translation_mm[1]));
    g.position = add(g.position, scale(g.slice_dir, translation_mm[2]));

    auto rotate_frame = [&](const Vec3& axis, double theta) {
        if (theta == 0.0) return;
        g.read_dir  = rotate_about(g.read_dir,  axis, theta);
        g.phase_dir = rotate_about(g.phase_dir, axis, theta);
        g.slice_dir = rotate_about(g.slice_dir, axis, theta);
    };
    rotate_frame(g.read_dir,  rotation_rad[0]);
    rotate_frame(g.phase_dir, rotation_rad[1]);
    rotate_frame(g.slice_dir, rotation_rad[2]);

    const Mat3 clean = orthonormalize_rows(rot_from_geometry(g));
    return geometry_from_rot(clean, g.position);
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

// Axis-mapping switches settled by the one-time scanner check (the mapping
// between ISMRMRD header directions and the sequence's sROT_MATRIX rows is
// not provable from available source). Defaults = pass-through.
struct AxisOptions {
    bool transpose{false};        // send R^T instead of R
    bool swap_read_phase{false};  // exchange read_dir and phase_dir
    Vec3 axis_sign{1, 1, 1};      // per-PCS-axis sign applied to position and dirs
};

inline Geometry apply_axis_options(const Geometry& in, const AxisOptions& o) {
    Geometry g = in;
    if (o.swap_read_phase) {
        std::swap(g.read_dir, g.phase_dir);
        // keep right-handed: normal = read x phase
        g.slice_dir = cross(g.read_dir, g.phase_dir);
    }
    for (int i = 0; i < 3; ++i) {
        g.position[i]  *= o.axis_sign[i];
        g.read_dir[i]  *= o.axis_sign[i];
        g.phase_dir[i] *= o.axis_sign[i];
        g.slice_dir[i] *= o.axis_sign[i];
    }
    return g;
}

// Absolute geometry -> agent wire command. Caller must have checked the
// frame is right-handed (is_right_handed) — a left-handed frame has no
// Euler representation and would silently come out mirrored.
inline WireCommand command_from_geometry(const Geometry& geom,
                                         const AxisOptions& opts = {}) {
    const Geometry g = apply_axis_options(geom, opts);
    Mat3 R = orthonormalize_rows(rot_from_geometry(g));
    if (opts.transpose) R = transpose(R);
    double rx, ry, rz;
    euler_zyx_from_rot(R, rx, ry, rz);
    WireCommand c;
    c.tx = g.position[0] + 0.0;   // "+ 0.0" folds -0.0 into 0.0
    c.ty = g.position[1] + 0.0;
    c.tz = g.position[2] + 0.0;
    c.rx = rx * kRad2Deg + 0.0;
    c.ry = ry * kRad2Deg + 0.0;
    c.rz = rz * kRad2Deg + 0.0;
    c.flags = kFlagUpdate;
    return c;
}

} // namespace slice_math
