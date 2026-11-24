#pragma once

// Cameras around the origin and the analytic shapes the mesh-lab tests
// reconstruct: a ViewSet built directly (no dataset on disk), a unit sphere
// and the [-1, 1]^3 cube with its exact ray hits and normals.

#include "mesh/MeshViews.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace synth {

// A pinhole at `C` looking at the origin, CV axes (+Y down), world +Z up.
inline meshing::MeshView look_at(const double C[3], int w = 640, int h = 480, double f = 420) {
    meshing::MeshView v;
    v.cam.model = 0;
    v.cam.width = w; v.cam.height = h;
    v.cam.fx = v.cam.fy = f; v.cam.cx = w / 2.0; v.cam.cy = h / 2.0;
    for (int k = 0; k < 3; ++k) v.C[k] = C[k];
    double z[3] = {-C[0], -C[1], -C[2]};
    const double zl = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    for (double& x : z) x /= zl;
    double up[3] = {0, 0, 1};
    if (std::fabs(z[2]) > 0.95) { up[1] = 1; up[2] = 0; }
    const double d = up[0] * z[0] + up[1] * z[1] + up[2] * z[2];
    double y[3] = {-(up[0] - d * z[0]), -(up[1] - d * z[1]), -(up[2] - d * z[2])};
    const double yl = std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
    for (double& x : y) x /= yl;
    const double x[3] = {y[1] * z[2] - y[2] * z[1], y[2] * z[0] - y[0] * z[2], y[0] * z[1] - y[1] * z[0]};
    for (int k = 0; k < 3; ++k) { v.R[k] = x[k]; v.R[3 + k] = y[k]; v.R[6 + k] = z[k]; }
    return v;
}

// `n` cameras spread over a sphere of `radius` (a Fibonacci lattice).
inline meshing::ViewSet ring(int n, double radius) {
    meshing::ViewSet vs;
    const double golden = 3.14159265358979323846 * (3 - std::sqrt(5.0));
    for (int i = 0; i < n; ++i) {
        const double z = 1 - 2 * (i + 0.5) / n, r = std::sqrt(1 - z * z), a = golden * i;
        const double C[3] = {radius * r * std::cos(a), radius * r * std::sin(a), radius * z};
        vs.views.push_back(look_at(C));
    }
    return vs;
}

// Distance from p to the surface of the [-1, 1]^3 cube.
inline double cube_distance(const double p[3]) {
    double q[3], outside = 0, inside = -1e300;
    for (int k = 0; k < 3; ++k) {
        q[k] = std::fabs(p[k]) - 1;
        outside += std::max(q[k], 0.0) * std::max(q[k], 0.0);
        inside = std::max(inside, q[k]);
    }
    return outside > 0 ? std::sqrt(outside) : std::fabs(inside);
}

// The cube's first hit along o + t d (slab test) and its outward normal.
inline bool cube_hit(const double o[3], const double d[3], double& t, double n[3]) {
    double t0 = -1e300, t1 = 1e300;
    int axis = -1;
    double sign = 0;
    for (int k = 0; k < 3; ++k) {
        if (std::fabs(d[k]) < 1e-15) {
            if (std::fabs(o[k]) > 1) return false;
            continue;
        }
        double a = (-1 - o[k]) / d[k], b = (1 - o[k]) / d[k];
        double sa = -1;
        if (a > b) { std::swap(a, b); sa = 1; }
        if (a > t0) { t0 = a; axis = k; sign = sa; }
        t1 = std::min(t1, b);
    }
    if (t0 > t1 || t0 <= 0 || axis < 0) return false;
    t = t0;
    n[0] = n[1] = n[2] = 0;
    n[axis] = sign;
    return true;
}

}  // namespace synth
