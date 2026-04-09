/*
 * clients/kspace_streamer/phantom.hpp
 *
 * Shepp-Logan phantom generator + real 2D FFT (kissfft-backed) for the C++
 * k-space streamer. Matches the Python numpy.fft-backed path in the
 * Python scanner client - image_to_kspace does fftshift(fft2(ifftshift(img))) /
 * sqrt(nx*ny), same formula ismrmrdtools.transform.transform_image_to_kspace
 * uses.
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

#include "kiss_fftnd.h"

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

// Centered 2D FFT using kissfft. Matches Python numpy.fft path used by the
// ismrmrdtools generator:
//     k = fftshift(fft2(ifftshift(img))) / sqrt(nx*ny)
// Image is row-major (ny x nx), real double. Output k-space is row-major
// (ny x nx), complex<float>, DC term at (nx/2, ny/2).
inline void image_to_kspace(const std::vector<double>& img,
                            std::size_t nx, std::size_t ny,
                            std::vector<std::complex<float>>& out) {
    const std::size_t n = nx * ny;
    out.assign(n, {0.0f, 0.0f});

    // Build the ifftshifted complex input: origin moves to (0, 0).
    std::vector<kiss_fft_cpx> in(n);
    const std::size_t hx = nx / 2;
    const std::size_t hy = ny / 2;
    for (std::size_t y = 0; y < ny; ++y) {
        const std::size_t ys = (y + hy) % ny;
        for (std::size_t x = 0; x < nx; ++x) {
            const std::size_t xs = (x + hx) % nx;
            const double v = img[ys * nx + xs];
            kiss_fft_cpx& c = in[y * nx + x];
            c.r = static_cast<kiss_fft_scalar>(v);
            c.i = 0;
        }
    }

    // 2D forward FFT. dims: [ny, nx] (kissfft takes major-to-minor order).
    int dims[2] = {static_cast<int>(ny), static_cast<int>(nx)};
    kiss_fftnd_cfg cfg = kiss_fftnd_alloc(dims, 2, /*is_inverse=*/0, nullptr, nullptr);

    std::vector<kiss_fft_cpx> freq(n);
    kiss_fftnd(cfg, in.data(), freq.data());
    kiss_fft_free(cfg);

    // Apply fftshift on output and normalize by 1/sqrt(n).
    const double norm = 1.0 / std::sqrt(static_cast<double>(n));
    for (std::size_t y = 0; y < ny; ++y) {
        const std::size_t ys = (y + hy) % ny;
        for (std::size_t x = 0; x < nx; ++x) {
            const std::size_t xs = (x + hx) % nx;
            const kiss_fft_cpx& c = freq[y * nx + x];
            out[ys * nx + xs] = std::complex<float>(
                static_cast<float>(c.r * norm),
                static_cast<float>(c.i * norm));
        }
    }
}

} // namespace kspace_sim
