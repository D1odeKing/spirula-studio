#include "mesh/NormalRefine.h"

#include "data/Knn.h"
#include "mesh/MeshDistance.h"
#include "mesh/MeshParallel.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>

namespace meshing {

namespace {

constexpr double kPi = 3.14159265358979323846;

inline void sub3(const double* a, const double* b, double* o) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
inline double dot3(const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline void cross3(const double* a, const double* b, double* o) {
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}
inline bool normalize3(double* v) {
    const double l = std::sqrt(dot3(v, v));
    if (!(l > 0)) return false;
    v[0] /= l; v[1] /= l; v[2] /= l;
    return true;
}

struct Geometry {
    std::vector<double> n, c, a;   // per face: unit normal, centroid, area
};

void face_geometry(const std::vector<double>& X, const MeshData& mesh, Geometry& g) {
    const size_t nf = mesh.F.size();
    g.n.resize(nf * 3); g.c.resize(nf * 3); g.a.resize(nf);
    for (size_t f = 0; f < nf; ++f) {
        const double* p = &X[(size_t)mesh.F[f][0] * 3];
        const double* q = &X[(size_t)mesh.F[f][1] * 3];
        const double* r = &X[(size_t)mesh.F[f][2] * 3];
        double u[3], v[3], cr[3];
        sub3(q, p, u); sub3(r, p, v); cross3(u, v, cr);
        const double l = std::sqrt(dot3(cr, cr));
        g.a[f] = 0.5 * l;
        for (int k = 0; k < 3; ++k) {
            g.n[f * 3 + k] = l > 0 ? cr[k] / l : 0.0;
            g.c[f * 3 + k] = (p[k] + q[k] + r[k]) / 3.0;
        }
    }
}

// Compressed rows: vertex -> faces, and face -> faces sharing a vertex.
struct Adjacency {
    std::vector<int64_t> vbeg, fbeg;
    std::vector<int32_t> vf, ff;
};

void build_adjacency(const MeshData& mesh, Adjacency& adj) {
    const size_t nv = mesh.V.size(), nf = mesh.F.size();
    adj.vbeg.assign(nv + 1, 0);
    for (const auto& f : mesh.F) for (int k = 0; k < 3; ++k) ++adj.vbeg[(size_t)f[k] + 1];
    for (size_t i = 0; i < nv; ++i) adj.vbeg[i + 1] += adj.vbeg[i];
    adj.vf.resize((size_t)adj.vbeg[nv]);
    std::vector<int64_t> at(adj.vbeg.begin(), adj.vbeg.end() - 1);
    for (size_t f = 0; f < nf; ++f) for (int k = 0; k < 3; ++k) adj.vf[(size_t)at[(size_t)mesh.F[f][k]]++] = (int32_t)f;
    adj.fbeg.assign(nf + 1, 0);
    std::vector<int32_t> ring;
    for (size_t f = 0; f < nf; ++f) {
        ring.clear();
        for (int k = 0; k < 3; ++k) {
            const int v = mesh.F[f][k];
            for (int64_t j = adj.vbeg[(size_t)v]; j < adj.vbeg[(size_t)v + 1]; ++j) ring.push_back(adj.vf[(size_t)j]);
        }
        std::sort(ring.begin(), ring.end());
        ring.erase(std::unique(ring.begin(), ring.end()), ring.end());
        adj.ff.insert(adj.ff.end(), ring.begin(), ring.end());
        adj.fbeg[f + 1] = (int64_t)adj.ff.size();
    }
}

bool solve3(const double M[9], const double b[3], double x[3]) {
    const double d = M[0] * (M[4] * M[8] - M[5] * M[7]) - M[1] * (M[3] * M[8] - M[5] * M[6]) +
                     M[2] * (M[3] * M[7] - M[4] * M[6]);
    if (!(std::fabs(d) > 1e-300)) return false;
    double c0[3] = {M[0], M[3], M[6]}, c1[3] = {M[1], M[4], M[7]}, c2[3] = {M[2], M[5], M[8]};
    auto det = [](const double* a, const double* bb, const double* c) {
        return a[0] * (bb[1] * c[2] - bb[2] * c[1]) - bb[0] * (a[1] * c[2] - a[2] * c[1]) + c[0] * (a[1] * bb[2] - a[2] * bb[1]);
    };
    x[0] = det(b, c1, c2) / d;
    x[1] = det(c0, b, c2) / d;
    x[2] = det(c0, c1, b) / d;
    return true;
}

}  // namespace

void guide_samples_for_view(const MeshData& mesh, const MeshView& view, const NormalMap& map,
                            int raster, std::vector<GuideSample>& out) {
    const int W = view.cam.width, H = view.cam.height;
    if (W <= 0 || H <= 0) return;
    const double sc = std::min(1.0, (double)std::max(16, raster) / std::max(W, H));
    const int rw = std::max(1, (int)std::ceil(W * sc)), rh = std::max(1, (int)std::ceil(H * sc));
    const size_t nv = mesh.V.size(), nf = mesh.F.size();
    std::vector<double> sx(nv * 2), sz(nv);
    std::vector<uint8_t> ok(nv);
    for (size_t i = 0; i < nv; ++i) {
        const double X[3] = {mesh.V[i][0], mesh.V[i][1], mesh.V[i][2]};
        double px[2], depth = 0;
        ok[i] = project(view, X, px, &depth) && depth > 0;
        sx[i * 2] = px[0] * sc; sx[i * 2 + 1] = px[1] * sc; sz[i] = depth;
    }
    std::vector<float> zbuf((size_t)rw * rh, 1e30f);
    std::vector<int32_t> fid((size_t)rw * rh, -1);
    for (size_t f = 0; f < nf; ++f) {
        const int a = mesh.F[f][0], b = mesh.F[f][1], c = mesh.F[f][2];
        if (!ok[(size_t)a] || !ok[(size_t)b] || !ok[(size_t)c]) continue;
        const double x0 = sx[(size_t)a * 2], y0 = sx[(size_t)a * 2 + 1], x1 = sx[(size_t)b * 2], y1 = sx[(size_t)b * 2 + 1];
        const double x2 = sx[(size_t)c * 2], y2 = sx[(size_t)c * 2 + 1];
        const double area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (!(std::fabs(area) > 1e-12)) continue;
        const int bx0 = std::max(0, (int)std::floor(std::min({x0, x1, x2})));
        const int bx1 = std::min(rw - 1, (int)std::ceil(std::max({x0, x1, x2})));
        const int by0 = std::max(0, (int)std::floor(std::min({y0, y1, y2})));
        const int by1 = std::min(rh - 1, (int)std::ceil(std::max({y0, y1, y2})));
        // A face spanning half the frame is a wide lens folding it: not a sample.
        if (bx1 - bx0 > rw / 2 || by1 - by0 > rh / 2) continue;
        for (int y = by0; y <= by1; ++y)
            for (int x = bx0; x <= bx1; ++x) {
                const double px = x + 0.5, py = y + 0.5;
                const double w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) / area;
                const double w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) / area;
                const double w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const float z = (float)(w0 * sz[(size_t)a] + w1 * sz[(size_t)b] + w2 * sz[(size_t)c]);
                const size_t p = (size_t)y * rw + x;
                if (z < zbuf[p]) { zbuf[p] = z; fid[p] = (int32_t)f; }
            }
    }
    std::vector<float> sum(nf * 3, 0.0f);
    std::vector<int32_t> cnt(nf, 0);
    for (int y = 0; y < rh; ++y)
        for (int x = 0; x < rw; ++x) {
            const int32_t f = fid[(size_t)y * rw + x];
            if (f < 0) continue;
            const double px[2] = {(x + 0.5) / sc, (y + 0.5) / sc};
            float n[3];
            if (!map.sample(view, px, n)) continue;
            for (int k = 0; k < 3; ++k) sum[(size_t)f * 3 + k] += n[k];
            ++cnt[(size_t)f];
        }
    for (size_t f = 0; f < nf; ++f) {
        if (!cnt[f]) continue;
        double nc[3] = {sum[f * 3], sum[f * 3 + 1], sum[f * 3 + 2]}, nw[3];
        // The texel under the centroid, when this face owns it: a mean over
        // the face's pixels blends both sides of a crease at its edge.
        {
            const int a = mesh.F[f][0], b = mesh.F[f][1], c = mesh.F[f][2];
            const int x = (int)std::floor((sx[(size_t)a * 2] + sx[(size_t)b * 2] + sx[(size_t)c * 2]) / 3);
            const int y = (int)std::floor((sx[(size_t)a * 2 + 1] + sx[(size_t)b * 2 + 1] + sx[(size_t)c * 2 + 1]) / 3);
            float n[3];
            const double px[2] = {(x + 0.5) / sc, (y + 0.5) / sc};
            if (x >= 0 && y >= 0 && x < rw && y < rh && fid[(size_t)y * rw + x] == (int32_t)f && map.sample(view, px, n))
                for (int k = 0; k < 3; ++k) nc[k] = n[k];
        }
        if (!normalize3(nc)) continue;
        camera_dir_to_file(view, nc, nw);
        double cen[3] = {0, 0, 0}, to_cam[3], fn[3];
        const double* P[3];
        double pts[3][3];
        for (int k = 0; k < 3; ++k) {
            for (int j = 0; j < 3; ++j) pts[k][j] = mesh.V[(size_t)mesh.F[f][k]][(size_t)j];
            P[k] = pts[k];
            for (int j = 0; j < 3; ++j) cen[j] += pts[k][j] / 3.0;
        }
        sub3(view.C, cen, to_cam);
        const double dist = std::sqrt(dot3(to_cam, to_cam));
        if (!(dist > 0)) continue;
        for (double& v : to_cam) v /= dist;
        if (dot3(nw, to_cam) < 0) for (double& v : nw) v = -v;
        double u[3], v[3];
        sub3(P[1], P[0], u); sub3(P[2], P[0], v); cross3(u, v, fn);
        if (!normalize3(fn)) continue;
        GuideSample s;
        s.face = (int32_t)f;
        for (int k = 0; k < 3; ++k) s.n[k] = (float)nw[k];
        s.w = (float)(std::fabs(dot3(fn, to_cam)) * view.cam.fx / dist);
        if (s.w > 0) out.push_back(s);
    }
}

void robust_guides(int64_t faces, const std::vector<GuideSample>& samples, float outlier_deg,
                   std::vector<float>& guide, std::vector<float>& conf) {
    guide.assign((size_t)faces * 3, 0.0f);
    conf.assign((size_t)faces, 0.0f);
    std::vector<int64_t> beg((size_t)faces + 1, 0);
    for (const auto& s : samples) ++beg[(size_t)s.face + 1];
    for (int64_t f = 0; f < faces; ++f) beg[(size_t)f + 1] += beg[(size_t)f];
    std::vector<int64_t> at(beg.begin(), beg.end() - 1);
    std::vector<const GuideSample*> by((size_t)beg[(size_t)faces]);
    for (const auto& s : samples) by[(size_t)at[(size_t)s.face]++] = &s;
    const double cos_out = std::cos(outlier_deg * kPi / 180.0);
    parallel_for(faces, 0, [&](int64_t b, int64_t e) {
        for (int64_t f = b; f < e; ++f) {
            const int64_t k0 = beg[(size_t)f], k1 = beg[(size_t)f + 1];
            if (k0 == k1) continue;
            // Medoid by weighted angle: a median direction one bad view cannot drag.
            int64_t med = k0;
            double best = 1e300, wall = 0;
            const int64_t cand_end = std::min(k1, k0 + 64);
            for (int64_t j = k0; j < k1; ++j) wall += by[(size_t)j]->w;
            for (int64_t j = k0; j < cand_end; ++j) {
                double cost = 0;
                for (int64_t k = k0; k < k1; ++k) {
                    const float* a = by[(size_t)j]->n; const float* c = by[(size_t)k]->n;
                    const double d = std::max(-1.0, std::min(1.0, (double)a[0] * c[0] + (double)a[1] * c[1] + (double)a[2] * c[2]));
                    cost += by[(size_t)k]->w * std::acos(d);
                }
                if (cost < best) { best = cost; med = j; }
            }
            double m[3] = {0, 0, 0}, wk = 0;
            int kept = 0;
            const float* mn = by[(size_t)med]->n;
            for (int64_t k = k0; k < k1; ++k) {
                const float* c = by[(size_t)k]->n;
                if ((double)mn[0] * c[0] + (double)mn[1] * c[1] + (double)mn[2] * c[2] < cos_out) continue;
                for (int j = 0; j < 3; ++j) m[j] += by[(size_t)k]->w * c[j];
                wk += by[(size_t)k]->w;
                ++kept;
            }
            const double R = std::sqrt(dot3(m, m));
            if (!(R > 0) || !(wk > 0)) continue;
            for (int j = 0; j < 3; ++j) guide[(size_t)f * 3 + j] = (float)(m[j] / R);
            // Agreement (mean resultant length) x share of the weight kept, and
            // a quarter for a face only one view saw.
            const double agree = R / wk, share = wk / wall;
            conf[(size_t)f] = (float)(agree * agree * share * (kept >= 2 ? 1.0 : 0.25));
        }
    });
}

void refine_with_normals(MeshData& mesh, const ViewSet& views, const std::vector<double>& anchors,
                         const RefineConfig& cfg, RefineStats& stats, std::vector<float>& shift,
                         const std::function<void(const char*)>& progress) {
    auto stage = [&](const char* s) { if (progress) progress(s); };
    stats = RefineStats{};
    const size_t nv = mesh.V.size(), nf = mesh.F.size();
    shift.assign(nv, 0.0f);
    if (nv == 0 || nf == 0) return;

    // ---- guides: one z-buffer per view with a normal map ----
    stage("guides");
    std::vector<GuideSample> samples;
    std::mutex lock;
    std::atomic<int> with_maps{0};
    parallel_for((int64_t)views.views.size(), cfg.threads, [&](int64_t b, int64_t e) {
        std::vector<GuideSample> local;
        for (int64_t i = b; i < e; ++i) {
            const MeshView& v = views.views[(size_t)i];
            if (v.normal.empty()) continue;
            NormalMap map;
            std::string error;
            if (!load_normal_map(v.normal, map, error)) continue;
            ++with_maps;
            guide_samples_for_view(mesh, v, map, cfg.raster, local);
        }
        std::lock_guard<std::mutex> g(lock);
        samples.insert(samples.end(), local.begin(), local.end());
    });
    stats.views = with_maps.load();
    std::vector<float> guide, conf;
    robust_guides((int64_t)nf, samples, cfg.outlier_deg, guide, conf);
    samples.clear(); samples.shrink_to_fit();

    std::vector<double> X(nv * 3), X0;
    for (size_t i = 0; i < nv; ++i) for (int k = 0; k < 3; ++k) X[i * 3 + k] = mesh.V[i][(size_t)k];
    X0 = X;
    Adjacency adj;
    build_adjacency(mesh, adj);
    Geometry g;
    face_geometry(X, mesh, g);
    double conf_sum = 0, angle_sum = 0;
    for (size_t f = 0; f < nf; ++f) {
        if (!(conf[f] > 0)) continue;
        ++stats.guided;
        conf_sum += conf[f];
        // A guide faces its camera; the mesh may wind either way. Take its side.
        const double gd = guide[f * 3] * g.n[f * 3] + guide[f * 3 + 1] * g.n[f * 3 + 1] + guide[f * 3 + 2] * g.n[f * 3 + 2];
        if (gd < 0) for (int k = 0; k < 3; ++k) guide[f * 3 + k] = -guide[f * 3 + k];
        angle_sum += std::acos(std::min(1.0, std::fabs(gd))) * 180.0 / kPi;
    }
    stats.mean_conf = stats.guided ? conf_sum / stats.guided : 0;
    stats.guide_angle_deg = stats.guided ? angle_sum / stats.guided : 0;

    double edge = 0;
    for (const auto& f : mesh.F)
        for (int k = 0; k < 3; ++k) {
            double d[3];
            sub3(&X[(size_t)f[(size_t)k] * 3], &X[(size_t)f[(size_t)((k + 1) % 3)] * 3], d);
            edge += std::sqrt(dot3(d, d));
        }
    edge /= 3.0 * nf;
    const double cap = cfg.max_shift > 0 ? cfg.max_shift : edge;
    stats.cap = cap;

    // ---- anchors: the input vertex, moved along its normal to the nearest
    // dense point when one is within the cap ----
    std::vector<double> A = X0;
    {
        std::vector<double> vn(nv * 3, 0.0);
        for (size_t f = 0; f < nf; ++f)
            for (int k = 0; k < 3; ++k)
                for (int j = 0; j < 3; ++j) vn[(size_t)mesh.F[f][(size_t)k] * 3 + j] += g.n[f * 3 + j] * g.a[f];
        if (!anchors.empty()) {
            const int64_t na = (int64_t)anchors.size() / 3;
            std::vector<float> fa((size_t)na * 3);
            for (int64_t i = 0; i < na * 3; ++i) fa[(size_t)i] = (float)(anchors[(size_t)i] - anchors[(size_t)(i % 3)]);
            const knn::KdTree3 tree(fa.data(), na);
            parallel_for((int64_t)nv, cfg.threads, [&](int64_t b, int64_t e) {
                for (int64_t i = b; i < e; ++i) {
                    double n[3] = {vn[(size_t)i * 3], vn[(size_t)i * 3 + 1], vn[(size_t)i * 3 + 2]};
                    if (!normalize3(n)) continue;
                    const float q[3] = {(float)(X0[(size_t)i * 3] - anchors[0]), (float)(X0[(size_t)i * 3 + 1] - anchors[1]),
                                        (float)(X0[(size_t)i * 3 + 2] - anchors[2])};
                    float d2;
                    int32_t j;
                    if (!tree.query(q, -1, 1, &d2, &j) || std::sqrt((double)d2) > cap) continue;
                    double d[3];
                    sub3(&anchors[(size_t)j * 3], &X0[(size_t)i * 3], d);
                    const double off = dot3(d, n);
                    for (int k = 0; k < 3; ++k) A[(size_t)i * 3 + k] = X0[(size_t)i * 3 + k] + off * n[k];
                }
            });
        }
    }

    const double sigma_s = std::max(1e-30, cfg.sigma_s * edge);
    const double inv2s = 1.0 / (2 * sigma_s * sigma_s), inv2r = 1.0 / (2.0 * cfg.sigma_r * cfg.sigma_r);
    std::vector<double> nf_cur, nf_next(nf * 3), Xn(nv * 3);
    for (int round = 0; round < std::max(1, cfg.rounds); ++round) {
        stage("filter");
        face_geometry(X, mesh, g);
        nf_cur = g.n;
        double amean = 0;
        for (double a : g.a) amean += a / nf;
        if (!(amean > 0)) break;
        for (int it = 0; it < cfg.normal_iters; ++it) {
            parallel_for((int64_t)nf, cfg.threads, [&](int64_t b, int64_t e) {
                for (int64_t f = b; f < e; ++f) {
                    double s[3] = {0, 0, 0};
                    const bool gf = conf[(size_t)f] > 0;
                    for (int64_t j = adj.fbeg[(size_t)f]; j < adj.fbeg[(size_t)f + 1]; ++j) {
                        const int32_t h = adj.ff[(size_t)j];
                        double dc[3];
                        sub3(&g.c[(size_t)f * 3], &g.c[(size_t)h * 3], dc);
                        const bool gh = conf[(size_t)h] > 0;
                        // The range signal is the guide where both faces have one:
                        // its discontinuities are the creases to keep.
                        double dr2 = 0;
                        for (int k = 0; k < 3; ++k) {
                            const double r = gf && gh ? (double)guide[(size_t)f * 3 + k] - guide[(size_t)h * 3 + k]
                                                      : nf_cur[(size_t)f * 3 + k] - nf_cur[(size_t)h * 3 + k];
                            dr2 += r * r;
                        }
                        const double w = g.a[(size_t)h] * std::exp(-dot3(dc, dc) * inv2s - dr2 * inv2r);
                        for (int k = 0; k < 3; ++k) s[k] += w * nf_cur[(size_t)h * 3 + k];
                    }
                    if (!normalize3(s)) { for (int k = 0; k < 3; ++k) s[k] = nf_cur[(size_t)f * 3 + k]; }
                    if (gf) {
                        const double t = std::min(1.0, (double)cfg.pull * conf[(size_t)f]);
                        for (int k = 0; k < 3; ++k) s[k] = (1 - t) * s[k] + t * guide[(size_t)f * 3 + k];
                        if (!normalize3(s)) for (int k = 0; k < 3; ++k) s[k] = guide[(size_t)f * 3 + k];
                    }
                    for (int k = 0; k < 3; ++k) nf_next[(size_t)f * 3 + k] = s[k];
                }
            });
            std::swap(nf_cur, nf_next);
        }

        // ---- vertex solve: per vertex, the 3x3 normal equations of
        // sum A_f (n_f . (x_i - x_j))^2 + lambda |x_i - a_i|^2, damped Jacobi ----
        stage("vertices");
        for (int it = 0; it < cfg.vertex_iters; ++it) {
            parallel_for((int64_t)nv, cfg.threads, [&](int64_t b, int64_t e) {
                for (int64_t i = b; i < e; ++i) {
                    double M[9] = {cfg.lambda, 0, 0, 0, cfg.lambda, 0, 0, 0, cfg.lambda};
                    double r[3] = {cfg.lambda * A[(size_t)i * 3], cfg.lambda * A[(size_t)i * 3 + 1], cfg.lambda * A[(size_t)i * 3 + 2]};
                    for (int64_t j = adj.vbeg[(size_t)i]; j < adj.vbeg[(size_t)i + 1]; ++j) {
                        const int32_t f = adj.vf[(size_t)j];
                        const double* n = &nf_cur[(size_t)f * 3];
                        const double w = g.a[(size_t)f] / amean;
                        double nn[9];
                        for (int p = 0; p < 3; ++p) for (int q = 0; q < 3; ++q) nn[p * 3 + q] = w * n[p] * n[q];
                        for (int k = 0; k < 3; ++k) {
                            const int v = mesh.F[(size_t)f][(size_t)k];
                            if (v == (int)i) continue;
                            for (int p = 0; p < 9; ++p) M[p] += nn[p];
                            for (int p = 0; p < 3; ++p)
                                r[p] += nn[p * 3] * X[(size_t)v * 3] + nn[p * 3 + 1] * X[(size_t)v * 3 + 1] + nn[p * 3 + 2] * X[(size_t)v * 3 + 2];
                        }
                    }
                    double x[3];
                    if (!solve3(M, r, x)) { for (int k = 0; k < 3; ++k) x[k] = X[(size_t)i * 3 + k]; }
                    double d[3];
                    for (int k = 0; k < 3; ++k) d[k] = 0.5 * (x[k] + X[(size_t)i * 3 + k]) - X0[(size_t)i * 3 + k];
                    const double l = std::sqrt(dot3(d, d));
                    if (l > cap) for (double& v : d) v *= cap / l;
                    for (int k = 0; k < 3; ++k) Xn[(size_t)i * 3 + k] = X0[(size_t)i * 3 + k] + d[k];
                }
            });
            std::swap(X, Xn);
        }
    }

    // ---- planes: regions of one guide direction, snapped when their anchors
    // agree with one plane to within plane_tol ----
    if (cfg.plane_snap) {
        stage("planes");
        const double tol = cfg.plane_tol > 0 ? cfg.plane_tol : cap / 4;
        const double cos_p = std::cos(cfg.plane_deg * kPi / 180.0);
        std::vector<int32_t> region(nf, -1);
        std::vector<int32_t> stack, members;
        std::vector<uint8_t> in_region(nf, 0);
        for (size_t seed = 0; seed < nf; ++seed) {
            if (region[seed] >= 0 || conf[seed] < 0.5f) continue;
            members.clear();
            stack.assign(1, (int32_t)seed);
            region[seed] = (int32_t)seed;
            double m[3] = {guide[seed * 3], guide[seed * 3 + 1], guide[seed * 3 + 2]};
            while (!stack.empty()) {
                const int32_t f = stack.back();
                stack.pop_back();
                members.push_back(f);
                double mu[3] = {m[0], m[1], m[2]};
                normalize3(mu);
                for (int64_t j = adj.fbeg[(size_t)f]; j < adj.fbeg[(size_t)f + 1]; ++j) {
                    const int32_t h = adj.ff[(size_t)j];
                    if (region[(size_t)h] >= 0 || conf[(size_t)h] < 0.5f) continue;
                    const double c = mu[0] * guide[(size_t)h * 3] + mu[1] * guide[(size_t)h * 3 + 1] + mu[2] * guide[(size_t)h * 3 + 2];
                    if (c < cos_p) continue;
                    region[(size_t)h] = (int32_t)seed;
                    for (int k = 0; k < 3; ++k) m[k] += guide[(size_t)h * 3 + k];
                    stack.push_back(h);
                }
            }
            if ((int)members.size() < cfg.plane_min_faces) continue;
            for (int32_t f : members) in_region[(size_t)f] = 1;
            std::vector<int32_t> verts;
            for (int32_t f : members) for (int k = 0; k < 3; ++k) verts.push_back(mesh.F[(size_t)f][(size_t)k]);
            std::sort(verts.begin(), verts.end());
            verts.erase(std::unique(verts.begin(), verts.end()), verts.end());
            double c[3] = {0, 0, 0}, C[9] = {};
            for (int32_t v : verts) for (int k = 0; k < 3; ++k) c[k] += A[(size_t)v * 3 + k] / verts.size();
            for (int32_t v : verts) {
                double d[3];
                sub3(&A[(size_t)v * 3], c, d);
                for (int p = 0; p < 3; ++p) for (int q = 0; q < 3; ++q) C[p * 3 + q] += d[p] * d[q];
            }
            double n[3];
            smallest_eigenvector(C, n);
            double sq = 0;
            for (int32_t v : verts) {
                double d[3];
                sub3(&A[(size_t)v * 3], c, d);
                sq += dot3(d, n) * dot3(d, n);
            }
            const bool flat = std::sqrt(sq / verts.size()) < tol;
            if (flat) {
                ++stats.planes;
                for (int32_t v : verts) {
                    bool interior = true;
                    for (int64_t j = adj.vbeg[(size_t)v]; j < adj.vbeg[(size_t)v + 1]; ++j)
                        interior &= in_region[(size_t)adj.vf[(size_t)j]] != 0;
                    if (!interior) continue;
                    double* x = &X[(size_t)v * 3];
                    double d[3];
                    sub3(x, c, d);
                    const double off = dot3(d, n);
                    double moved[3];
                    for (int k = 0; k < 3; ++k) moved[k] = x[k] - off * n[k] - X0[(size_t)v * 3 + k];
                    const double l = std::sqrt(dot3(moved, moved));
                    if (l > cap) for (double& t : moved) t *= cap / l;
                    for (int k = 0; k < 3; ++k) x[k] = X0[(size_t)v * 3 + k] + moved[k];
                    ++stats.snapped;
                }
            }
            for (int32_t f : members) in_region[(size_t)f] = 0;
        }
    }

    // ---- report: the move along each vertex's final normal ----
    face_geometry(X, mesh, g);
    std::vector<double> vn(nv * 3, 0.0);
    for (size_t f = 0; f < nf; ++f)
        for (int k = 0; k < 3; ++k)
            for (int j = 0; j < 3; ++j) vn[(size_t)mesh.F[f][(size_t)k] * 3 + j] += g.n[f * 3 + j] * g.a[f];
    double total = 0;
    for (size_t i = 0; i < nv; ++i) {
        double d[3], n[3] = {vn[i * 3], vn[i * 3 + 1], vn[i * 3 + 2]};
        sub3(&X[i * 3], &X0[i * 3], d);
        const double l = std::sqrt(dot3(d, d));
        total += l;
        stats.max_shift = std::max(stats.max_shift, l);
        shift[i] = normalize3(n) ? (float)dot3(d, n) : (float)l;
        for (int k = 0; k < 3; ++k) mesh.V[i][(size_t)k] = (float)X[i * 3 + k];
    }
    stats.mean_shift = total / nv;
    mesh.N.clear();
}

}  // namespace meshing
