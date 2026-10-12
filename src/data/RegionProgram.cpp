// RegionProgram.cpp -- see RegionProgram.h; mirrors shaders/region.slang.

#include "data/RegionProgram.h"

#include "data/LabelField.h"
#include "data/Region.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <numeric>

namespace spirula {

void RegionProgram::push(int type, const double* c, double r, const double* half,
                         const double* rotation, int label) {
    float node[4 * kNodeStride] = {};
    node[0] = (float)type;
    node[3] = (float)label;
    const double s = xf_scale;
    if (c && type == 2) {
        for (int k = 0; k < 3; k++) node[4 + k] = (float)c[k];
        node[7] = (float)(s * r - (c[0] * xf_shift[0] + c[1] * xf_shift[1] + c[2] * xf_shift[2]));
    } else if (c) {
        for (int k = 0; k < 3; k++) node[4 + k] = (float)(s * c[k] + xf_shift[k]);
        node[7] = (float)(s * r);
    }
    if (half)
        for (int k = 0; k < 3; k++) node[8 + k] = (float)(s * half[k]);
    const double identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const double* R = rotation ? rotation : identity;
    for (int row = 0; row < 3; row++)
        for (int k = 0; k < 3; k++) node[12 + row * 4 + k] = (float)R[row * 3 + k];
    nodes.insert(nodes.end(), node, node + 4 * kNodeStride);
}

void RegionProgram::push_prism(const double* center, const double* rotation, double half_height,
                               const std::vector<double>& polygon) {
    const int nv = (int)(polygon.size() / 2);
    const double half[3] = {0, 0, half_height};
    push(12, center, 0, half, rotation);
    nodes[nodes.size() - 4 * kNodeStride + 1] = (float)nv;
    const size_t at = nodes.size();
    nodes.resize(at + (size_t)payload_nodes(nv) * 4 * kNodeStride, 0.0f);
    for (int i = 0; i < 2 * nv; i++) nodes[at + (size_t)i] = (float)(xf_scale * polygon[(size_t)i]);
}

void RegionProgram::apply_similarity(double scale, const double shift[3]) {
    for (int i = 0; i < num_nodes(); i++) {
        float* h = &nodes[(size_t)i * 4 * kNodeStride];
        const int type = (int)h[0];
        if (type == 0 || type == 1 || (type >= 10 && type <= 12)) {
            for (int k = 0; k < 3; k++) h[4 + k] = (float)(scale * h[4 + k] + shift[k]);
            if (type == 1)
                h[7] = (float)(scale * h[7]);
            else
                for (int k = 0; k < 3; k++) h[8 + k] = (float)(scale * h[8 + k]);
            if (type == 12) {
                const int nv = (int)h[1];
                float* poly = h + 4 * kNodeStride;
                for (int k = 0; k < 2 * nv; k++) poly[k] = (float)(scale * poly[k]);
                i += payload_nodes(nv);
            }
        } else if (type == 2) {
            h[7] = (float)(scale * h[7] - (h[4] * shift[0] + h[5] * shift[1] + h[6] * shift[2]));
        }
    }
    move_field(scale, shift);
}

void RegionProgram::move_field(double scale, const double shift[3]) {
    if (field) {
        auto moved = std::make_shared<LabelField>(*field);
        for (int64_t i = 0; i < moved->num_seeds(); i++)
            for (int k = 0; k < 3; k++) {
                float& v = moved->seeds[(size_t)i * 4 + k];
                v = (float)(scale * v + shift[k]);
            }
        for (int64_t i = 0; i < moved->num_nodes(); i++)
            for (int k = 0; k < 3; k++) {
                float& lo = moved->nodes[(size_t)i * 8 + k];
                float& hi = moved->nodes[(size_t)i * 8 + 4 + k];
                lo = (float)(scale * lo + shift[k]);
                hi = (float)(scale * hi + shift[k]);
            }
        field = moved;
    }
}

bool compile_region(const Region& r, RegionProgram& out, std::string& error, double scale,
                    const double* shift) {
    out = RegionProgram{};
    out.xf_scale = scale;
    if (shift)
        for (int k = 0; k < 3; k++) out.xf_shift[k] = shift[k];
    if (!r.emit(out, error)) return false;
    if (scale != 1.0 || out.xf_shift[0] != 0.0 || out.xf_shift[1] != 0.0 || out.xf_shift[2] != 0.0)
        out.move_field(scale, out.xf_shift);
    return true;
}

bool Region::emit(RegionProgram&, std::string& error) const {
    error = std::string("region kind '") + kind() + "' has no device form";
    return false;
}

bool BoxRegion::emit(RegionProgram& out, std::string&) const {
    out.push(0, center, 0, half, R);
    return true;
}

bool SphereRegion::emit(RegionProgram& out, std::string&) const {
    out.push(1, center, radius);
    return true;
}

bool EllipsoidRegion::emit(RegionProgram& out, std::string&) const {
    out.push(10, center, 0, half, R);
    return true;
}

bool CylinderRegion::emit(RegionProgram& out, std::string&) const {
    out.push(11, center, 0, half, R);
    return true;
}

bool PrismRegion::emit(RegionProgram& out, std::string& error) const {
    if (polygon.size() < 6) {
        error = "prism needs at least three vertices";
        return false;
    }
    out.push_prism(center, R, half_height, polygon);
    return true;
}

bool HalfSpaceRegion::emit(RegionProgram& out, std::string&) const {
    out.push(2, normal, offset);
    return true;
}

bool CsgRegion::emit(RegionProgram& out, std::string& error) const {
    const int op = op_code();
    if (children.empty()) {
        out.push(op == 5 ? 9 : (op == 7 ? 8 : 9));
        return true;
    }
    if (!children[0]->emit(out, error)) return false;
    if (op == 7) {
        out.push(7);
        return true;
    }
    // n-ary folded left: a op b op c ...
    for (size_t i = 1; i < children.size(); i++) {
        if (!children[i]->emit(out, error)) return false;
        out.push(op);
    }
    return true;
}

// ---- host evaluation --------------------------------------------------------

bool program_contains(const RegionProgram& p, const double point[3], const double* normal) {
    bool stack[32];
    int sp = 0;
    const int n = p.num_nodes();
    const float* nodes = p.nodes.data();
    for (int i = 0; i < n; i++) {
        const float* h = nodes + (size_t)i * 4 * RegionProgram::kNodeStride;
        const int type = (int)h[0];
        bool v = false;
        if (type == 0 || (type >= 10 && type <= 12)) {
            const float* c = h + 4;
            const float* half = h + 8;
            const double d[3] = {point[0] - c[0], point[1] - c[1], point[2] - c[2]};
            double q[3];
            for (int row = 0; row < 3; row++) {
                const float* rr = h + 12 + row * 4;
                q[row] = rr[0] * d[0] + rr[1] * d[1] + rr[2] * d[2];
            }
            if (type == 0) {
                v = std::fabs(q[0]) <= half[0] && std::fabs(q[1]) <= half[1] &&
                    std::fabs(q[2]) <= half[2];
            } else if (type == 10) {
                double s = 0;
                for (int k = 0; k < 3; k++) s += (q[k] / half[k]) * (q[k] / half[k]);
                v = s <= 1.0;
            } else if (type == 11) {
                v = std::fabs(q[2]) <= half[2] &&
                    (q[0] / half[0]) * (q[0] / half[0]) + (q[1] / half[1]) * (q[1] / half[1]) <= 1.0;
            } else {
                const int nv = (int)h[1];
                const float* poly = h + 4 * RegionProgram::kNodeStride;
                v = false;
                if (std::fabs(q[2]) <= half[2])
                    for (int a = 0, b = nv - 1; a < nv; b = a++) {
                        const double xa = poly[2 * a], ya = poly[2 * a + 1];
                        const double xb = poly[2 * b], yb = poly[2 * b + 1];
                        if ((ya > q[1]) != (yb > q[1]) && q[0] < (xb - xa) * (q[1] - ya) / (yb - ya) + xa)
                            v = !v;
                    }
                i += RegionProgram::payload_nodes(nv);
            }
        } else if (type == 1) {
            const float* c = h + 4;
            const double d[3] = {point[0] - c[0], point[1] - c[1], point[2] - c[2]};
            v = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] <= (double)c[3] * c[3];
        } else if (type == 2) {
            const float* c = h + 4;
            v = c[0] * point[0] + c[1] * point[1] + c[2] * point[2] + c[3] >= 0.0;
        } else if (type == 3) {
            v = p.field && p.field->label(point, normal) == (int)h[3];
        } else if (type == 8) {
            v = true;
        } else if (type == 9) {
            v = false;
        } else if (type == 7) {
            if (sp > 0) v = !stack[--sp];
        } else {
            const bool b = sp > 0 ? stack[--sp] : false;
            const bool a = sp > 0 ? stack[--sp] : false;
            v = type == 4 ? (a || b) : (type == 5 ? (a && b) : (a && !b));
        }
        if (sp < 32) stack[sp++] = v;
    }
    return sp > 0 && stack[sp - 1];
}

void splat_normal(const float quat[4], const float log_scale[3], const double toward[3],
                  double out[3]) {
    double w = quat[0], x = quat[1], y = quat[2], z = quat[3];
    const double len = std::sqrt(w * w + x * x + y * y + z * z);
    if (len > 1e-20) { w /= len; x /= len; y /= len; z /= len; }
    const double cols[3][3] = {
        {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)},
        {2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)},
        {2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)}};
    int a = 0;
    if (log_scale[1] < log_scale[0] && log_scale[1] <= log_scale[2]) a = 1;
    else if (log_scale[2] < log_scale[0] && log_scale[2] < log_scale[1]) a = 2;
    for (int k = 0; k < 3; k++) out[k] = cols[a][k];
    if (out[0] * toward[0] + out[1] * toward[1] + out[2] * toward[2] < 0)
        for (int k = 0; k < 3; k++) out[k] = -out[k];
}

// ---- splat sampling ---------------------------------------------------------

namespace {

void quat_columns(const float quat[4], float c[3][3]) {
    float w = quat[0], x = quat[1], y = quat[2], z = quat[3];
    const float inv = 1.0f / std::sqrt(std::max(w * w + x * x + y * y + z * z, 1e-20f));
    w *= inv; x *= inv; y *= inv; z *= inv;
    const float cols[3][3] = {
        {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)},
        {2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)},
        {2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)}};
    std::memcpy(c, cols, sizeof(cols));
}

uint32_t region_hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

}  // namespace

std::vector<float> region_sample_pattern(int num_samples) {
    const int m = std::clamp(num_samples, 1, kMaxRegionSamples) - 1;
    std::vector<float> out((size_t)m * 4, 0.0f);
    if (m == 0) return out;
    // Radii at the stratum midpoints of chi(3), the length of a 3D standard
    // normal, dealt to the directions in golden-ratio order.
    const double pi = 3.14159265358979323846;
    auto chi3_quantile = [&](double q) {
        double lo = 0, hi = 10;
        for (int it = 0; it < 60; it++) {
            const double r = 0.5 * (lo + hi);
            const double cdf = std::erf(r / std::sqrt(2.0)) -
                               std::sqrt(2.0 / pi) * r * std::exp(-0.5 * r * r);
            (cdf < q ? lo : hi) = r;
        }
        return 0.5 * (lo + hi);
    };
    std::vector<int> order((size_t)m);
    std::iota(order.begin(), order.end(), 0);
    auto key = [](int k) { return std::fmod(k * 0.6180339887498949, 1.0); };
    std::sort(order.begin(), order.end(), [&](int a, int b) { return key(a) < key(b); });
    std::vector<double> radius((size_t)m);
    for (int i = 0; i < m; i++) radius[(size_t)order[(size_t)i]] = chi3_quantile((i + 0.5) / m);
    std::vector<double> pts((size_t)m * 3);
    double mean[3] = {0, 0, 0};
    for (int k = 0; k < m; k++) {
        const double z = 1.0 - (2.0 * k + 1.0) / m, phi = k * pi * (3.0 - std::sqrt(5.0));
        const double rho = std::sqrt(std::max(0.0, 1.0 - z * z)), r = radius[(size_t)k];
        const double p[3] = {r * rho * std::cos(phi), r * rho * std::sin(phi), r * z};
        for (int a = 0; a < 3; a++) {
            pts[(size_t)k * 3 + a] = p[a];
            mean[a] += p[a] / m;
        }
    }
    // Centred, and scaled so the m + 1 points with the splat's centre have the
    // Gaussian's mean and E|z|^2 = 3: the midpoints shorten the tail. Centring
    // lifts the planar guarantee at 8 points from 25% of the mass inside to 20%.
    double sum_r2 = 0;
    for (int k = 0; k < m; k++)
        for (int a = 0; a < 3; a++) {
            if (m >= 2) pts[(size_t)k * 3 + a] -= mean[a];
            sum_r2 += pts[(size_t)k * 3 + a] * pts[(size_t)k * 3 + a];
        }
    const double scale = std::sqrt(3.0 * (m + 1) / sum_r2);
    for (int k = 0; k < m; k++)
        for (int a = 0; a < 3; a++) out[(size_t)k * 4 + a] = (float)(scale * pts[(size_t)k * 3 + a]);
    return out;
}

bool splat_contains(const RegionProgram& p, const float mean[3], const float quat[4],
                    const float log_scale[3], const double* normal,
                    const std::vector<float>& pattern, uint32_t index, uint32_t seed) {
    const double centre[3] = {mean[0], mean[1], mean[2]};
    if (program_contains(p, centre, normal)) return true;
    const size_t num_pattern = pattern.size() / 4;
    if (num_pattern == 0) return false;
    const uint32_t h0 = region_hash(index ^ region_hash(seed));
    const uint32_t h1 = region_hash(h0 + 0x9e3779b9u);
    const uint32_t h2 = region_hash(h1 + 0x9e3779b9u);
    const float u = (float)(h0 >> 8) * (1.0f / 16777216.0f);
    const float a = (float)(h1 >> 8) * (6.2831853f / 16777216.0f);
    const float b = (float)(h2 >> 8) * (6.2831853f / 16777216.0f);
    const float ra = std::sqrt(1.0f - u), rb = std::sqrt(u);
    const float turn[4] = {ra * std::sin(a), ra * std::cos(a), rb * std::sin(b), rb * std::cos(b)};
    float t[3][3], c[3][3], axes[3][3];
    quat_columns(turn, t);
    quat_columns(quat, c);
    for (int i = 0; i < 3; i++)
        for (int r = 0; r < 3; r++) c[i][r] *= std::exp(log_scale[i]);
    for (int j = 0; j < 3; j++)
        for (int r = 0; r < 3; r++)
            axes[j][r] = c[0][r] * t[j][0] + c[1][r] * t[j][1] + c[2][r] * t[j][2];
    for (size_t k = 0; k < num_pattern; k++) {
        const float* z = &pattern[k * 4];
        double q[3];
        for (int r = 0; r < 3; r++)
            q[r] = mean[r] + axes[0][r] * z[0] + axes[1][r] * z[1] + axes[2][r] * z[2];
        if (program_contains(p, q, normal)) return true;
    }
    return false;
}

}  // namespace spirula
