#pragma once

// Spherical-beta lobes on the host: the lobe colour (the reference the
// device copy, shaders/spherical_beta.slang, is tested against), turning a
// lobe with its model, and the values a fresh lobe starts at.
// docs/notes/spherical-beta.md.

#include <cmath>

namespace spirula::sb {

// One lobe as it is stored: (r, g, b) raw amplitude, theta, phi, beta.
constexpr int kLobeFloats = 6;
constexpr double kSoftplusBeta = 6.9314718055994530;   // 10 ln 2
constexpr double kPi = 3.14159265358979323846;

inline double softplus(double x) {
    const double bx = kSoftplusBeta * x;
    return (std::fmax(bx, 0.0) + std::log1p(std::exp(-std::fabs(bx)))) / kSoftplusBeta;
}

inline void axis(double theta, double phi, double out[3]) {
    const double st = std::sin(theta);
    out[0] = st * std::cos(phi);
    out[1] = st * std::sin(phi);
    out[2] = std::cos(theta);
}

// The colour one lobe adds for unit view direction `dir`.
inline void lobe_color(const float lobe[kLobeFloats], const double dir[3],
                       double out[3]) {
    double a[3];
    axis(lobe[3], lobe[4], a);
    const double d = dir[0] * a[0] + dir[1] * a[1] + dir[2] * a[2];
    const double w = d > 0.0 ? std::pow(d, 4.0 * std::exp((double)lobe[5])) : 0.0;
    for (int c = 0; c < 3; c++) out[c] = w * softplus(lobe[c]);
}

// Turn a lobe by the row-major rotation R: its axis becomes R axis, written
// back as angles. theta lands in [0, pi] and phi in (-pi, pi] -- the same
// axis, which is all the colour depends on.
inline void rotate_lobe(const double R[9], float lobe[kLobeFloats]) {
    double a[3], b[3];
    axis(lobe[3], lobe[4], a);
    for (int r = 0; r < 3; r++)
        b[r] = R[3 * r] * a[0] + R[3 * r + 1] * a[1] + R[3 * r + 2] * a[2];
    const double z = std::fmin(std::fmax(b[2], -1.0), 1.0);
    lobe[3] = (float)std::acos(z);
    lobe[4] = (float)std::atan2(b[1], b[0]);
}

// A fresh lobe: amplitude 0 (0.1 after the softplus) and sharpness 0
// (exponent 4), as the reference trainers seed it, but the axis uniform on
// the sphere from the caller's u, v in [0, 1) where they draw theta uniform.
inline void init_lobe(float u, float v, float lobe[kLobeFloats]) {
    lobe[0] = lobe[1] = lobe[2] = 0.0f;
    lobe[3] = (float)std::acos(1.0 - 2.0 * (double)u);
    lobe[4] = (float)(2.0 * kPi * v);
    lobe[5] = 0.0f;
}

// A lobe that adds nothing a float can show: softplus(-10) = 1.4e-31. What a
// row gains when a model is raised to more lobes than it was trained with.
inline void dark_lobe(float lobe[kLobeFloats]) {
    lobe[0] = lobe[1] = lobe[2] = -10.0f;
    lobe[3] = lobe[4] = lobe[5] = 0.0f;
}

}  // namespace spirula::sb
