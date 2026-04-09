/*
 * clients/kspace_streamer/phantom.hpp
 *
 * Shepp-Logan phantom generator + naive 2D DFT for the C++ k-space streamer.
 * Header-only, no external deps beyond <complex>, <cmath>, <vector>. Naive
 * O(n^4) DFT is fine for demo sizes (e.g. 128x128). If we ever need larger
 * matrices or higher frame rates, swap this for FFTW / kissfft.
 *
 * The ellipse parameters are the standard Shepp-Logan head phantom (Shepp &
 * Logan, 1974) - the same constants every implementation uses, including
 * Python's ismrmrdtools.simulation.phantom().
 */

#pragma once

#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace kspace_sim {

struct Ellipse {
    double x0, y0, a, b, theta, intensity;
};

inline const std::vector<Ellipse>& shepp_logan_ellipses() {
    static const std::vector<Ellipse> kEllipses = {
        { 0.0000,  0.0000, 0.6900, 0.9200,   0.0,  1.00},
        { 0.0000, -0.0184, 0.6624, 0.8740,   0.0, -0.80},
        { 0.2200,  0.0000, 0.1100, 0.3100, -18.0, -0.20},
        {-0.2200,  0.0000, 0.1600, 0.4100,  18.0, -0.20},
        { 0.0000,  0.3500, 0.2100, 0.2500,   0.0,  0.10},
        { 0.0000,  0.1000, 0.0460, 0.0460,   0.0,  0.10},
        { 0.0000, -0.1000, 0.0460, 0.0460,   0.0,  0.10},
        {-0.0800, -0.6050, 0.0460, 0.0230,   0.0,  0.10},
        { 0.0000, -0.6060, 0.0230, 0.0230,   0.0,  0.10},
        { 0.0600, -0.6050, 0.0230, 0.0460,   0.0,  0.10},
    };
    return kEllipses;
}

// Render a Shepp-Logan phantom into `out` (size nx*ny, row-major).
// rotation_rad rotates all ellipses around the origin.
// brightness scales every ellipse's intensity uniformly.
inline void build_shepp_logan(std::size_t nx, std::size_t ny,
                              double rotation_rad, double brightness,
                              std::vector<double>& out) {
    out.assign(nx * ny, 0.0);
    const auto& ellipses = shepp_logan_ellipses();
    for (const auto& e : ellipses) {
        const double th = e.theta * M_PI / 180.0 + rotation_rad;
        const double ct = std::cos(th);
        const double st = std::sin(th);
        const double a2 = e.a * e.a;
        const double b2 = e.b * e.b;
        for (std::size_t y = 0; y < ny; ++y) {
            const double yn = 2.0 * (static_cast<double>(y) / (ny - 1)) - 1.0;
            for (std::size_t x = 0; x < nx; ++x) {
                const double xn = 2.0 * (static_cast<double>(x) / (nx - 1)) - 1.0;
                const double dx = (xn - e.x0) * ct + (yn - e.y0) * st;
                const double dy = -(xn - e.x0) * st + (yn - e.y0) * ct;
                if ((dx * dx) / a2 + (dy * dy) / b2 <= 1.0) {
                    out[y * nx + x] += brightness * e.intensity;
                }
            }
        }
    }
}

// Naive centered 2D DFT. Image is row-major (ny x nx), real double.
// Output k-space is row-major (ny x nx), complex<float>, DC at (nx/2, ny/2).
// Normalization is 1/sqrt(nx*ny).
inline void image_to_kspace(const std::vector<double>& img,
                            std::size_t nx, std::size_t ny,
                            std::vector<std::complex<float>>& out) {
    out.assign(nx * ny, {0.0f, 0.0f});
    const double norm = 1.0 / std::sqrt(static_cast<double>(nx * ny));
    for (std::size_t ky = 0; ky < ny; ++ky) {
        const double fy = static_cast<double>(ky) - static_cast<double>(ny) / 2.0;
        for (std::size_t kx = 0; kx < nx; ++kx) {
            const double fx = static_cast<double>(kx) - static_cast<double>(nx) / 2.0;
            std::complex<double> acc{0.0, 0.0};
            for (std::size_t y = 0; y < ny; ++y) {
                const double ys = static_cast<double>(y) - static_cast<double>(ny) / 2.0;
                for (std::size_t x = 0; x < nx; ++x) {
                    const double xs = static_cast<double>(x) - static_cast<double>(nx) / 2.0;
                    const double phase =
                        -2.0 * M_PI * (fx * xs / static_cast<double>(nx) +
                                       fy * ys / static_cast<double>(ny));
                    acc += img[y * nx + x] *
                           std::complex<double>(std::cos(phase), std::sin(phase));
                }
            }
            acc *= norm;
            out[ky * nx + kx] = std::complex<float>(
                static_cast<float>(acc.real()),
                static_cast<float>(acc.imag()));
        }
    }
}

} // namespace kspace_sim
