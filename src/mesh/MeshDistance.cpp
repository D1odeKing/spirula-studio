#include "mesh/MeshDistance.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace meshing {

namespace {

inline double dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Ericson, Real-Time Collision Detection, 5.1.5: the Voronoi region of q.
void closest_on_triangle(const double q[3], const double* t, double out[3]) {
    const double* a = t; const double* b = t + 3; const double* c = t + 6;
    double ab[3], ac[3], ap[3];
    for (int k = 0; k < 3; ++k) { ab[k] = b[k] - a[k]; ac[k] = c[k] - a[k]; ap[k] = q[k] - a[k]; }
    const double d1 = dot3(ab, ap), d2 = dot3(ac, ap);
    if (d1 <= 0 && d2 <= 0) { std::copy_n(a, 3, out); return; }
    double bp[3];
    for (int k = 0; k < 3; ++k) bp[k] = q[k] - b[k];
    const double d3 = dot3(ab, bp), d4 = dot3(ac, bp);
    if (d3 >= 0 && d4 <= d3) { std::copy_n(b, 3, out); return; }
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        const double v = d1 / (d1 - d3);
        for (int k = 0; k < 3; ++k) out[k] = a[k] + v * ab[k];
        return;
    }
    double cp[3];
    for (int k = 0; k < 3; ++k) cp[k] = q[k] - c[k];
    const double d5 = dot3(ab, cp), d6 = dot3(ac, cp);
    if (d6 >= 0 && d5 <= d6) { std::copy_n(c, 3, out); return; }
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        const double w = d2 / (d2 - d6);
        for (int k = 0; k < 3; ++k) out[k] = a[k] + w * ac[k];
        return;
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        for (int k = 0; k < 3; ++k) out[k] = b[k] + w * (c[k] - b[k]);
        return;
    }
    const double denom = va + vb + vc;
    if (!(std::fabs(denom) > 0)) { std::copy_n(a, 3, out); return; }
    const double v = vb / denom, w = vc / denom;
    for (int k = 0; k < 3; ++k) out[k] = a[k] + ab[k] * v + ac[k] * w;
}

double box_d2(const float lo[3], const float hi[3], const double q[3]) {
    double d2 = 0;
    for (int k = 0; k < 3; ++k) {
        const double d = q[k] < lo[k] ? lo[k] - q[k] : q[k] > hi[k] ? q[k] - hi[k] : 0.0;
        d2 += d * d;
    }
    return d2;
}

}  // namespace

TriangleBvh::TriangleBvh(const MeshData& mesh) {
    const int nf = (int)mesh.F.size();
    tris_.resize((size_t)nf * 9);
    std::vector<double> centroid((size_t)nf * 3);
    for (int f = 0; f < nf; ++f)
        for (int c = 0; c < 3; ++c) {
            const auto& p = mesh.V[(size_t)mesh.F[(size_t)f][c]];
            for (int k = 0; k < 3; ++k) {
                tris_[(size_t)f * 9 + c * 3 + k] = p[k];
                centroid[(size_t)f * 3 + k] += p[k] / 3.0;
            }
        }
    order_.resize((size_t)nf);
    std::iota(order_.begin(), order_.end(), 0);
    nodes_.reserve((size_t)std::max(1, 2 * nf / 4 + 1));
    if (nf > 0) build(0, nf, centroid);
}

int TriangleBvh::build(int first, int count, std::vector<double>& centroid) {
    const int id = (int)nodes_.size();
    nodes_.push_back({});
    Node n;
    for (int k = 0; k < 3; ++k) { n.lo[k] = 1e30f; n.hi[k] = -1e30f; }
    double clo[3] = {1e300, 1e300, 1e300}, chi[3] = {-1e300, -1e300, -1e300};
    for (int i = first; i < first + count; ++i) {
        const int f = order_[(size_t)i];
        for (int c = 0; c < 9; ++c) {
            const float v = (float)tris_[(size_t)f * 9 + c];
            const int k = c % 3;
            n.lo[k] = std::min(n.lo[k], std::nextafter(v, -1e30f));
            n.hi[k] = std::max(n.hi[k], std::nextafter(v, 1e30f));
        }
        for (int k = 0; k < 3; ++k) {
            clo[k] = std::min(clo[k], centroid[(size_t)f * 3 + k]);
            chi[k] = std::max(chi[k], centroid[(size_t)f * 3 + k]);
        }
    }
    if (count <= 4) {
        n.first = first;
        n.count = count;
        nodes_[(size_t)id] = n;
        return id;
    }
    int ax = 0;
    for (int k = 1; k < 3; ++k)
        if (chi[k] - clo[k] > chi[ax] - clo[ax]) ax = k;
    const int mid = first + count / 2;
    std::nth_element(order_.begin() + first, order_.begin() + mid, order_.begin() + first + count,
                     [&](int32_t a, int32_t b) { return centroid[(size_t)a * 3 + ax] < centroid[(size_t)b * 3 + ax]; });
    build(first, mid - first, centroid);
    n.count = 0;
    n.first = build(mid, first + count - mid, centroid);
    nodes_[(size_t)id] = n;
    return id;
}

double TriangleBvh::closest(const double q[3], double point[3], int* face) const {
    if (nodes_.empty()) return -1.0;
    double best = 1e300, best_p[3] = {0, 0, 0};
    int best_f = -1;
    int stack[128];
    int top = 0;
    stack[top++] = 0;
    while (top) {
        const int id = stack[--top];
        const Node& n = nodes_[(size_t)id];
        if (box_d2(n.lo, n.hi, q) >= best) continue;
        if (n.count) {
            for (int i = n.first; i < n.first + n.count; ++i) {
                const int f = order_[(size_t)i];
                double p[3];
                closest_on_triangle(q, &tris_[(size_t)f * 9], p);
                const double d2 = (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) +
                                  (p[2] - q[2]) * (p[2] - q[2]);
                if (d2 < best) { best = d2; best_f = f; std::copy_n(p, 3, best_p); }
            }
            continue;
        }
        const int l = id + 1, r = n.first;
        const double dl = box_d2(nodes_[(size_t)l].lo, nodes_[(size_t)l].hi, q);
        const double dr = box_d2(nodes_[(size_t)r].lo, nodes_[(size_t)r].hi, q);
        if (top + 2 > 128) continue;
        if (dl < dr) { stack[top++] = r; stack[top++] = l; }
        else { stack[top++] = l; stack[top++] = r; }
    }
    if (point) std::copy_n(best_p, 3, point);
    if (face) *face = best_f;
    return best;
}

void TriangleBvh::face_normal(int f, double n[3]) const {
    const double* t = &tris_[(size_t)f * 9];
    double u[3], v[3];
    for (int k = 0; k < 3; ++k) { u[k] = t[3 + k] - t[k]; v[k] = t[6 + k] - t[k]; }
    n[0] = u[1] * v[2] - u[2] * v[1];
    n[1] = u[2] * v[0] - u[0] * v[2];
    n[2] = u[0] * v[1] - u[1] * v[0];
    const double len = std::sqrt(dot3(n, n));
    if (len > 0) for (int k = 0; k < 3; ++k) n[k] /= len;
}

void smallest_eigenvector(double A[9], double out[3]) {
    double V[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (int sweep = 0; sweep < 16; ++sweep) {
        const double off = A[1] * A[1] + A[2] * A[2] + A[5] * A[5];
        if (off < 1e-30) break;
        for (int p = 0; p < 2; ++p)
            for (int q = p + 1; q < 3; ++q) {
                const double apq = A[p * 3 + q];
                if (std::fabs(apq) < 1e-300) continue;
                const double theta = (A[q * 3 + q] - A[p * 3 + p]) / (2 * apq);
                const double t = (theta >= 0 ? 1 : -1) / (std::fabs(theta) + std::sqrt(theta * theta + 1));
                const double c = 1 / std::sqrt(t * t + 1), s = t * c;
                for (int k = 0; k < 3; ++k) {
                    const double akp = A[k * 3 + p], akq = A[k * 3 + q];
                    A[k * 3 + p] = c * akp - s * akq;
                    A[k * 3 + q] = s * akp + c * akq;
                }
                for (int k = 0; k < 3; ++k) {
                    const double apk = A[p * 3 + k], aqk = A[q * 3 + k];
                    A[p * 3 + k] = c * apk - s * aqk;
                    A[q * 3 + k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 3; ++k) {
                    const double vkp = V[k * 3 + p], vkq = V[k * 3 + q];
                    V[k * 3 + p] = c * vkp - s * vkq;
                    V[k * 3 + q] = s * vkp + c * vkq;
                }
            }
    }
    int m = 0;
    for (int k = 1; k < 3; ++k)
        if (A[k * 4] < A[m * 4]) m = k;
    for (int k = 0; k < 3; ++k) out[k] = V[k * 3 + m];
}

double mesh_area(const MeshData& mesh) {
    double a = 0;
    for (const auto& f : mesh.F) {
        const auto &p = mesh.V[(size_t)f[0]], &q = mesh.V[(size_t)f[1]], &r = mesh.V[(size_t)f[2]];
        const double u[3] = {(double)q[0] - p[0], (double)q[1] - p[1], (double)q[2] - p[2]};
        const double v[3] = {(double)r[0] - p[0], (double)r[1] - p[1], (double)r[2] - p[2]};
        const double c[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        a += 0.5 * std::sqrt(dot3(c, c));
    }
    return a;
}

void sample_surface(const MeshData& mesh, int64_t n, uint32_t seed,
                    std::vector<double>& xyz, std::vector<int32_t>* face) {
    xyz.clear();
    if (face) face->clear();
    const size_t nf = mesh.F.size();
    if (nf == 0 || n <= 0) return;
    std::vector<double> cdf(nf);
    double acc = 0;
    for (size_t f = 0; f < nf; ++f) {
        const auto &p = mesh.V[(size_t)mesh.F[f][0]], &q = mesh.V[(size_t)mesh.F[f][1]], &r = mesh.V[(size_t)mesh.F[f][2]];
        const double u[3] = {(double)q[0] - p[0], (double)q[1] - p[1], (double)q[2] - p[2]};
        const double v[3] = {(double)r[0] - p[0], (double)r[1] - p[1], (double)r[2] - p[2]};
        const double c[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        acc += 0.5 * std::sqrt(dot3(c, c));
        cdf[f] = acc;
    }
    if (!(acc > 0)) return;
    uint64_t state = 0x9E3779B97F4A7C15ull ^ seed;
    auto uniform = [&state]() {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        return (double)(state >> 11) * (1.0 / 9007199254740992.0);
    };
    xyz.resize((size_t)n * 3);
    if (face) face->resize((size_t)n);
    for (int64_t i = 0; i < n; ++i) {
        const size_t f = (size_t)(std::lower_bound(cdf.begin(), cdf.end(), uniform() * acc) - cdf.begin());
        const size_t fi = std::min(f, nf - 1);
        double a = uniform(), b = uniform();
        if (a + b > 1) { a = 1 - a; b = 1 - b; }
        const auto &p = mesh.V[(size_t)mesh.F[fi][0]], &q = mesh.V[(size_t)mesh.F[fi][1]], &r = mesh.V[(size_t)mesh.F[fi][2]];
        for (int k = 0; k < 3; ++k) xyz[(size_t)i * 3 + k] = p[k] + a * ((double)q[k] - p[k]) + b * ((double)r[k] - p[k]);
        if (face) (*face)[(size_t)i] = (int32_t)fi;
    }
}

}  // namespace meshing
