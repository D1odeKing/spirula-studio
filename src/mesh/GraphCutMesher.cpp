#include "mesh/GraphCutMesher.h"

#include "data/Knn.h"
#include "mesh/Delaunay3D.h"
#include "mesh/MaxFlow.h"
#include "mesh/MeshClean.h"
#include "mesh/MeshDistance.h"
#include "mesh/MeshParallel.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace meshing {

namespace {

struct Cloud {
    std::vector<double> xyz;
    std::vector<uint8_t> rgb;
    int64_t size() const { return (int64_t)xyz.size() / 3; }
};

Cloud voxel_thin(const std::vector<double>& xyz, const std::vector<uint8_t>& rgb, double voxel) {
    const int64_t n = (int64_t)xyz.size() / 3;
    double lo[3] = {1e300, 1e300, 1e300};
    for (int64_t i = 0; i < n; ++i)
        for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], xyz[(size_t)i * 3 + k]);
    struct Acc { double p[3]; uint32_t c[3]; uint32_t count; };
    std::unordered_map<uint64_t, uint32_t> cell;
    cell.reserve((size_t)std::min<int64_t>(n, 1 << 24));
    std::vector<Acc> acc;
    for (int64_t i = 0; i < n; ++i) {
        uint64_t key = 0;
        for (int k = 0; k < 3; ++k) {
            const uint64_t q = (uint64_t)std::min(2097151.0, std::floor((xyz[(size_t)i * 3 + k] - lo[k]) / voxel));
            key |= q << (21 * k);
        }
        auto [it, fresh] = cell.emplace(key, (uint32_t)acc.size());
        if (fresh) acc.push_back(Acc{{0, 0, 0}, {0, 0, 0}, 0});
        Acc& a = acc[it->second];
        for (int k = 0; k < 3; ++k) {
            a.p[k] += xyz[(size_t)i * 3 + k];
            if (!rgb.empty()) a.c[k] += rgb[(size_t)i * 3 + k];
        }
        ++a.count;
    }
    Cloud out;
    out.xyz.resize(acc.size() * 3);
    if (!rgb.empty()) out.rgb.resize(acc.size() * 3);
    for (size_t j = 0; j < acc.size(); ++j)
        for (int k = 0; k < 3; ++k) {
            out.xyz[j * 3 + k] = acc[j].p[k] / acc[j].count;
            if (!rgb.empty()) out.rgb[j * 3 + k] = (uint8_t)(acc[j].c[k] / acc[j].count);
        }
    return out;
}

// The 1st-99th percentile box diagonal: one stray point should not size the voxel.
double robust_diagonal(const std::vector<double>& xyz) {
    const size_t n = xyz.size() / 3;
    double d2 = 0;
    for (int k = 0; k < 3; ++k) {
        std::vector<double> c;
        c.reserve(n / 16 + 1);
        for (size_t i = 0; i < n; i += 16) c.push_back(xyz[i * 3 + k]);
        std::sort(c.begin(), c.end());
        const double e = c[(size_t)(0.99 * (c.size() - 1))] - c[(size_t)(0.01 * (c.size() - 1))];
        d2 += e * e;
    }
    return std::sqrt(d2);
}

Cloud thin(const std::vector<double>& xyz, const std::vector<uint8_t>& rgb, const GraphCutConfig& cfg) {
    Cloud c = cfg.voxel > 0 ? voxel_thin(xyz, rgb, cfg.voxel) : Cloud{xyz, rgb};
    if (cfg.max_points <= 0 || c.size() <= cfg.max_points) return c;
    // A surface of area ~ diag^2 / 2 holds ~ that / v^2 points; start under
    // that guess and widen until the budget holds.
    double v = std::max(cfg.voxel, 0.5 * robust_diagonal(xyz) / std::sqrt(2.0 * (double)cfg.max_points));
    for (int pass = 0; pass < 40 && c.size() > cfg.max_points; ++pass, v *= 1.25) c = voxel_thin(xyz, rgb, v);
    return c;
}

inline void atomic_add(std::atomic<float>& a, float v) {
    float cur = a.load(std::memory_order_relaxed);
    while (!a.compare_exchange_weak(cur, cur + v, std::memory_order_relaxed)) {}
}

inline void sub3(const double* a, const double* b, double* o) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
inline double dot3(const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline void cross3(const double* a, const double* b, double* o) {
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}
inline double det3(const double* a, const double* b, const double* c) {
    double x[3];
    cross3(b, c, x);
    return dot3(a, x);
}

struct Tets {
    const double* P = nullptr;
    const uint32_t* cv = nullptr;
    const int* adj = nullptr;
    int64_t nt = 0;
    const double* pt(int64_t t, int k) const { return P + (size_t)cv[(size_t)t * 4 + k] * 3; }
    // Normal of facet f of tet t, pointing away from its opposite corner.
    void facet_normal(int64_t t, int f, double n[3]) const {
        const double* a = pt(t, (f + 1) & 3);
        const double* b = pt(t, (f + 2) & 3);
        const double* c = pt(t, (f + 3) & 3);
        double u[3], v[3], w[3];
        sub3(b, a, u);
        sub3(c, a, v);
        cross3(u, v, n);
        sub3(pt(t, f), a, w);
        if (dot3(n, w) > 0) for (int k = 0; k < 3; ++k) n[k] = -n[k];
    }
    int back_slot(int64_t n, int64_t t) const {
        for (int k = 0; k < 4; ++k)
            if (adj[(size_t)n * 4 + k] == (int)t) return k;
        return -1;
    }
};

// The tet around vertex v whose facet opposite v the ray v + s d leaves through:
// the one whose corner cone holds d best. -1 when no cone holds it, which on a
// hull vertex means the ray is outside at once.
int64_t start_tet(const Tets& T, int64_t seed, uint32_t v, const double d[3], int* local) {
    int64_t stack[512], seen[512];
    int top = 0, nseen = 0;
    stack[top++] = seed;
    seen[nseen++] = seed;
    int64_t best = -1;
    double best_score = -1e300;
    while (top) {
        const int64_t t = stack[--top];
        int l = -1;
        for (int k = 0; k < 4; ++k)
            if (T.cv[(size_t)t * 4 + k] == v) l = k;
        if (l < 0) continue;
        const double* p = T.pt(t, l);
        double e[3][3];
        for (int j = 0; j < 3; ++j) {
            sub3(T.pt(t, (l + 1 + j) & 3), p, e[j]);
            const double len = std::sqrt(dot3(e[j], e[j]));
            if (len > 0) for (double& x : e[j]) x /= len;
        }
        const double D = det3(e[0], e[1], e[2]);
        if (std::fabs(D) > 0) {
            const double a = det3(d, e[1], e[2]) / D, b = det3(e[0], d, e[2]) / D, c = det3(e[0], e[1], d) / D;
            const double score = std::min(a, std::min(b, c));
            if (score > best_score) { best_score = score; best = t; *local = l; }
        }
        for (int f = 0; f < 4; ++f) {
            if (f == l) continue;
            const int n = T.adj[(size_t)t * 4 + f];
            if (n < 0) continue;
            bool known = false;
            for (int s = 0; s < nseen; ++s) known |= seen[s] == n;
            if (known || nseen >= 512 || top >= 512) continue;
            seen[nseen++] = n;
            stack[top++] = n;
        }
    }
    return best_score > -1e-9 ? best : -1;
}

// Walks o + s d, s in [0, L], from tet t0 out through its facet `f0`.
// `cross(t, f, n)` sees each facet crossed; returns the tet that holds the end,
// or the last tet before the hull with *hull set.
template <typename Cross>
int64_t walk(const Tets& T, int64_t t0, int f0, const double o[3], const double d[3], double L,
             bool* hull, Cross&& cross) {
    *hull = false;
    int64_t cur = t0, prev = -1;
    int exit = f0;
    for (int step = 0; step < (1 << 20); ++step) {
        double n[3];
        T.facet_normal(cur, exit, n);
        const double den = dot3(d, n);
        double w[3];
        sub3(T.pt(cur, (exit + 1) & 3), o, w);
        const double s = den > 0 ? dot3(w, n) / den : 1e300;
        if (s >= L) return cur;
        const int64_t nxt = T.adj[(size_t)cur * 4 + exit];
        cross(cur, exit, nxt);
        if (nxt < 0) { *hull = true; return cur; }
        prev = cur;
        cur = nxt;
        // Exit of the next tet: the facet the ray leaves through first.
        int best = -1;
        double best_s = 1e300;
        for (int f = 0; f < 4; ++f) {
            if (T.adj[(size_t)cur * 4 + f] == (int)prev) continue;
            T.facet_normal(cur, f, n);
            const double dn = dot3(d, n);
            if (!(dn > 0)) continue;
            sub3(T.pt(cur, (f + 1) & 3), o, w);
            const double sf = dot3(w, n) / dn;
            if (sf < best_s) { best_s = sf; best = f; }
        }
        if (best < 0) return cur;
        exit = best;
    }
    return cur;
}

// The tets around edge (a, b), in order: once round, or hull to hull.
bool edge_ring(const Tets& T, int64_t t, uint32_t a, uint32_t b, std::vector<int64_t>& ring) {
    auto others = [&](int64_t tt, int& lc, int& ld) {
        lc = ld = -1;
        for (int k = 0; k < 4; ++k) {
            const uint32_t v = T.cv[(size_t)tt * 4 + k];
            if (v == a || v == b) continue;
            (lc < 0 ? lc : ld) = k;
        }
    };
    auto step = [&](int64_t prev, int64_t cur) {
        int lc, ld;
        others(cur, lc, ld);
        const int64_t n1 = T.adj[(size_t)cur * 4 + lc], n2 = T.adj[(size_t)cur * 4 + ld];
        return n1 == prev ? n2 : n1;
    };
    ring.assign(1, t);
    int lc, ld;
    others(t, lc, ld);
    int64_t prev = t, cur = T.adj[(size_t)t * 4 + ld];
    while (cur >= 0 && cur != t && ring.size() < 512) {
        ring.push_back(cur);
        const int64_t nxt = step(prev, cur);
        prev = cur;
        cur = nxt;
    }
    if (cur == t) return true;
    std::vector<int64_t> back;
    prev = t;
    cur = T.adj[(size_t)t * 4 + lc];
    while (cur >= 0 && back.size() < 512) {
        back.push_back(cur);
        const int64_t nxt = step(prev, cur);
        prev = cur;
        cur = nxt;
    }
    ring.insert(ring.begin(), back.rbegin(), back.rend());
    return false;
}

// Inside tets that meet along an edge and nowhere else make that edge
// non-manifold, which no mesh-level pass can split. Around each such edge the
// longest run of inside tets stays and the shorter runs go outside.
int64_t fix_nonmanifold_edges(const Tets& T, std::vector<uint8_t>& inside) {
    static const int kEdge[6][2] = {{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}};
    std::vector<int64_t> ring;
    int64_t flipped = 0;
    for (int pass = 0; pass < 8; ++pass) {
        int64_t changed = 0;
        for (int64_t t = 0; t < T.nt; ++t) {
            if (!inside[(size_t)t]) continue;
            for (const auto& e : kEdge) {
                if (!inside[(size_t)t]) break;
                const uint32_t a = T.cv[(size_t)t * 4 + e[0]], b = T.cv[(size_t)t * 4 + e[1]];
                const bool closed = edge_ring(T, t, a, b, ring);
                const size_t n = ring.size();
                // Runs of inside tets; an open ring has the outside at both ends.
                std::vector<std::pair<size_t, size_t>> runs;   // start, length
                size_t start = 0;
                if (closed) {
                    while (start < n && inside[(size_t)ring[start]]) ++start;
                    if (start == n) continue;   // inside all round: interior edge
                }
                for (size_t k = 0; k < n; ++k) {
                    const size_t i = (start + k) % n;
                    if (!inside[(size_t)ring[i]]) continue;
                    if (k == 0 || !inside[(size_t)ring[(start + k - 1) % n]]) runs.push_back({i, 0});
                    ++runs.back().second;
                }
                if (runs.size() <= 1) continue;
                size_t keep = 0;
                for (size_t r = 1; r < runs.size(); ++r)
                    if (runs[r].second > runs[keep].second) keep = r;
                for (size_t r = 0; r < runs.size(); ++r) {
                    if (r == keep) continue;
                    for (size_t k = 0; k < runs[r].second; ++k) inside[(size_t)ring[(runs[r].first + k) % n]] = 0;
                    changed += (int64_t)runs[r].second;
                }
            }
        }
        flipped += changed;
        if (!changed) break;
    }
    return flipped;
}

}  // namespace

MeshData graph_cut_mesh(const std::vector<double>& xyz_in, const std::vector<uint8_t>& rgb_in,
                        const ViewSet& views, const GraphCutConfig& cfg, GraphCutStats& stats,
                        const std::function<void(const char*)>& progress) {
    auto stage = [&](const char* s) { if (progress) progress(s); };
    stats = GraphCutStats{};
    stats.input_points = (int64_t)xyz_in.size() / 3;
    if (stats.input_points < 4) throw std::runtime_error("too few points to mesh");
    if (views.views.empty()) throw std::runtime_error("no cameras to mesh with");

    stage("thin");
    const Cloud cloud = thin(xyz_in, rgb_in, cfg);
    const int64_t n = cloud.size();
    stats.points = n;
    const double* P = cloud.xyz.data();

    // ---- local spacing and a PCA normal per point ----
    std::vector<float> fpts((size_t)n * 3);
    for (int64_t i = 0; i < n * 3; ++i) fpts[(size_t)i] = (float)(P[i] - P[i % 3]);
    const knn::KdTree3 tree(fpts.data(), n);
    std::vector<float> spacing((size_t)n), normal((size_t)n * 3);
    parallel_for(n, cfg.threads, [&](int64_t b, int64_t e) {
        constexpr int K = 10;
        float d2[K];
        int32_t idx[K];
        for (int64_t i = b; i < e; ++i) {
            const int got = tree.query(&fpts[(size_t)i * 3], (int32_t)i, K, d2, idx);
            double s = 0;
            const int nearest = std::min(got, 6);
            for (int j = 0; j < nearest; ++j) s += std::sqrt((double)d2[j]);
            spacing[(size_t)i] = nearest ? (float)(s / nearest) : 0.0f;
            double mu[3] = {P[i * 3], P[i * 3 + 1], P[i * 3 + 2]};
            for (int j = 0; j < got; ++j)
                for (int k = 0; k < 3; ++k) mu[k] += P[(size_t)idx[j] * 3 + k];
            for (int k = 0; k < 3; ++k) mu[k] /= got + 1;
            double C[9] = {};
            auto addp = [&](const double* q) {
                double dd[3];
                sub3(q, mu, dd);
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c) C[r * 3 + c] += dd[r] * dd[c];
            };
            addp(P + i * 3);
            for (int j = 0; j < got; ++j) addp(P + (size_t)idx[j] * 3);
            double v[3];
            smallest_eigenvector(C, v);
            for (int k = 0; k < 3; ++k) normal[(size_t)i * 3 + k] = (float)v[k];
        }
    });

    // ---- visibility: each point fills a disk of its spacing in a depth grid, so
    // an empty front cell cannot let the far side through (a third of a test
    // sphere's rays did, one cell per point); the most frontal views are kept ----
    stage("visibility");
    const int K = std::max(1, cfg.max_rays);
    std::vector<int32_t> ray_view((size_t)n * K, -1);
    std::vector<float> ray_score((size_t)n * K, -1.0f);
    std::vector<int32_t> cellof((size_t)n);
    std::vector<float> depthof((size_t)n);
    for (size_t vi = 0; vi < views.views.size(); ++vi) {
        const MeshView& v = views.views[vi];
        const double cell_px = (double)std::max(v.cam.width, v.cam.height) / std::max(8, cfg.grid);
        const int gw = (int)std::ceil(v.cam.width / cell_px), gh = (int)std::ceil(v.cam.height / cell_px);
        std::vector<std::atomic<float>> mind((size_t)gw * gh);
        for (auto& m : mind) m.store(1e30f, std::memory_order_relaxed);
        parallel_for(n, cfg.threads, [&](int64_t b, int64_t e) {
            for (int64_t i = b; i < e; ++i) {
                double px[2], depth = 0;
                cellof[(size_t)i] = -1;
                if (!project(v, P + i * 3, px, &depth) || !(depth > 0)) continue;
                const int cx = std::min(gw - 1, std::max(0, (int)(px[0] / cell_px)));
                const int cy = std::min(gh - 1, std::max(0, (int)(px[1] / cell_px)));
                cellof[(size_t)i] = cy * gw + cx;
                depthof[(size_t)i] = (float)depth;
                const double rc = 0.75 * spacing[(size_t)i] * v.cam.fx / depth / cell_px;
                const int ri = std::min(12, (int)std::ceil(rc));
                for (int dy = -ri; dy <= ri; ++dy)
                    for (int dx = -ri; dx <= ri; ++dx) {
                        const int x = cx + dx, y = cy + dy;
                        if (x < 0 || y < 0 || x >= gw || y >= gh || dx * dx + dy * dy > rc * rc + 0.5) continue;
                        std::atomic<float>& m = mind[(size_t)y * gw + x];
                        float cur = m.load(std::memory_order_relaxed);
                        while ((float)depth < cur && !m.compare_exchange_weak(cur, (float)depth, std::memory_order_relaxed)) {}
                    }
            }
        });
        parallel_for(n, cfg.threads, [&](int64_t b, int64_t e) {
            for (int64_t i = b; i < e; ++i) {
                const int c = cellof[(size_t)i];
                if (c < 0 || depthof[(size_t)i] > mind[(size_t)c].load(std::memory_order_relaxed) * (1 + cfg.depth_eps))
                    continue;
                double to_cam[3];
                sub3(v.C, P + i * 3, to_cam);
                const double len = std::sqrt(dot3(to_cam, to_cam));
                const float score = (float)std::fabs((to_cam[0] * normal[(size_t)i * 3] + to_cam[1] * normal[(size_t)i * 3 + 1] +
                                                      to_cam[2] * normal[(size_t)i * 3 + 2]) / len);
                int worst = 0;
                for (int k = 1; k < K; ++k)
                    if (ray_score[(size_t)i * K + k] < ray_score[(size_t)i * K + worst]) worst = k;
                if (score > ray_score[(size_t)i * K + worst]) {
                    ray_score[(size_t)i * K + worst] = score;
                    ray_view[(size_t)i * K + worst] = (int32_t)vi;
                }
            }
        });
    }
    cellof.clear(); cellof.shrink_to_fit();
    depthof.clear(); depthof.shrink_to_fit();

    // ---- Delaunay ----
    stage("delaunay");
    if (n > delaunay3d::kMaxPoints) throw std::runtime_error("too many points for the Delaunay triangulation");
    delaunay3d::Delaunay3DResult tri = delaunay3d::compute_delaunay_3d(P, (int)n, cfg.threads, true);
    Tets T;
    T.P = P;
    T.cv = tri.cell_vertices.data();
    T.adj = tri.cell_adjacents.data();
    T.nt = tri.nb_cells;
    stats.tets = T.nt;
    if (T.nt == 0) throw std::runtime_error("the points span no volume");
    std::vector<int64_t> vtet((size_t)n, -1);
    for (int64_t t = 0; t < T.nt; ++t)
        for (int k = 0; k < 4; ++k) vtet[T.cv[(size_t)t * 4 + k]] = t;

    // ---- ray energy ----
    stage("rays");
    std::unique_ptr<std::atomic<float>[]> fcap(new std::atomic<float>[(size_t)T.nt * 4]);
    std::unique_ptr<std::atomic<float>[]> src(new std::atomic<float>[(size_t)T.nt]);
    std::unique_ptr<std::atomic<float>[]> snk(new std::atomic<float>[(size_t)T.nt]);
    for (int64_t i = 0; i < T.nt * 4; ++i) fcap[(size_t)i].store(0.0f, std::memory_order_relaxed);
    for (int64_t i = 0; i < T.nt; ++i) {
        src[(size_t)i].store(0.0f, std::memory_order_relaxed);
        snk[(size_t)i].store(0.0f, std::memory_order_relaxed);
    }
    std::atomic<int64_t> rays{0};
    const float alpha = cfg.alpha_vis;
    parallel_for(n, cfg.threads, [&](int64_t b, int64_t e) {
        int64_t local_rays = 0;
        for (int64_t i = b; i < e; ++i) {
            if (vtet[(size_t)i] < 0) continue;
            for (int k = 0; k < K; ++k) {
                const int32_t vi = ray_view[(size_t)i * K + k];
                if (vi < 0) continue;
                const MeshView& v = views.views[(size_t)vi];
                double d[3];
                sub3(v.C, P + i * 3, d);
                const double L = std::sqrt(dot3(d, d));
                if (!(L > 0)) continue;
                for (double& x : d) x /= L;
                int l = -1;
                bool hull = false;
                // No tet: the ray leaves the hull at the point, into the outside
                // the source already stands for.
                const int64_t t0 = start_tet(T, vtet[(size_t)i], (uint32_t)i, d, &l);
                if (t0 >= 0) {
                    // A crossing from t toward the camera, into n: the arc n -> t
                    // (camera side outside, point side inside) is what it forbids.
                    const int64_t last = walk(T, t0, l, P + i * 3, d, L, &hull, [&](int64_t t, int, int64_t nb) {
                        if (nb < 0) return;
                        const int bs = T.back_slot(nb, t);
                        if (bs >= 0) atomic_add(fcap[(size_t)nb * 4 + bs], alpha);
                    });
                    atomic_add(src[(size_t)last], alpha);
                }
                const double back[3] = {-d[0], -d[1], -d[2]};
                const int64_t tb = start_tet(T, vtet[(size_t)i], (uint32_t)i, back, &l);
                if (tb >= 0) {
                    const int64_t behind = walk(T, tb, l, P + i * 3, back, cfg.sigma * spacing[(size_t)i], &hull,
                                                [](int64_t, int, int64_t) {});
                    if (!hull) atomic_add(snk[(size_t)behind], alpha);
                }
                ++local_rays;
            }
        }
        rays += local_rays;
    });
    stats.rays = rays.load();
    ray_view.clear(); ray_view.shrink_to_fit();
    ray_score.clear(); ray_score.shrink_to_fit();

    // ---- min cut: source = outside, sink = inside ----
    stage("mincut");
    MaxFlow<float> g((int)T.nt, T.nt * 4);
    for (int64_t t = 0; t < T.nt; ++t) {
        float hull_w = 0;
        for (int f = 0; f < 4; ++f) {
            const int nb = T.adj[(size_t)t * 4 + f];
            if (nb < 0) { hull_w += cfg.hull * alpha; continue; }
            if (nb < t) continue;
            const int bs = T.back_slot(nb, t);
            if (bs < 0) continue;
            float q = 0;
            if (cfg.quality > 0) {
                const double* a = T.pt(t, (f + 1) & 3);
                const double* bb = T.pt(t, (f + 2) & 3);
                const double* c = T.pt(t, (f + 3) & 3);
                double u[3], w[3], x[3], cr[3];
                sub3(bb, a, u); sub3(c, a, w); sub3(c, bb, x);
                const double l2 = dot3(u, u) + dot3(w, w) + dot3(x, x);
                cross3(u, w, cr);
                const double area = 0.5 * std::sqrt(dot3(cr, cr));
                const double perim = std::sqrt(dot3(u, u)) + std::sqrt(dot3(w, w)) + std::sqrt(dot3(x, x));
                const double s = (spacing[T.cv[(size_t)t * 4 + ((f + 1) & 3)]] + spacing[T.cv[(size_t)t * 4 + ((f + 2) & 3)]] +
                                  spacing[T.cv[(size_t)t * 4 + ((f + 3) & 3)]]) / 3.0 + 1e-30;
                const double bad = l2 > 0 ? 1.0 - std::min(1.0, 4.0 * std::sqrt(3.0) * area / l2) : 1.0;
                const double size = perim / (3.0 * s);
                q = (float)std::min(100.0, cfg.quality * size * size * (1.0 + bad)) * alpha;
            }
            g.add_edge((int)t, nb, fcap[(size_t)t * 4 + f].load() + q, fcap[(size_t)nb * 4 + bs].load() + q);
        }
        g.add_tweights((int)t, src[(size_t)t].load() + hull_w, snk[(size_t)t].load());
    }
    fcap.reset(); src.reset(); snk.reset();
    stats.flow = g.solve();
    std::vector<uint8_t> inside((size_t)T.nt);
    for (int64_t t = 0; t < T.nt; ++t) inside[(size_t)t] = !g.in_source((int)t);
    stats.relabelled = fix_nonmanifold_edges(T, inside);
    for (int64_t t = 0; t < T.nt; ++t) stats.inside += inside[(size_t)t];

    // ---- surface: every inside/outside facet, wound outward ----
    stage("extract");
    MeshData mesh;
    std::vector<int32_t> remap((size_t)n, -1);
    auto vid = [&](uint32_t p) {
        if (remap[p] < 0) {
            remap[p] = (int32_t)mesh.V.size();
            mesh.V.push_back({(float)P[(size_t)p * 3], (float)P[(size_t)p * 3 + 1], (float)P[(size_t)p * 3 + 2]});
            if (!cloud.rgb.empty())
                mesh.C.push_back({cloud.rgb[(size_t)p * 3], cloud.rgb[(size_t)p * 3 + 1], cloud.rgb[(size_t)p * 3 + 2]});
        }
        return remap[p];
    };
    for (int64_t t = 0; t < T.nt; ++t) {
        if (!inside[(size_t)t]) continue;
        for (int f = 0; f < 4; ++f) {
            const int nb = T.adj[(size_t)t * 4 + f];
            if (nb >= 0 && inside[(size_t)nb]) continue;
            uint32_t a = T.cv[(size_t)t * 4 + ((f + 1) & 3)], b = T.cv[(size_t)t * 4 + ((f + 2) & 3)],
                     c = T.cv[(size_t)t * 4 + ((f + 3) & 3)];
            double u[3], w[3], nn[3], o[3];
            sub3(P + (size_t)b * 3, P + (size_t)a * 3, u);
            sub3(P + (size_t)c * 3, P + (size_t)a * 3, w);
            cross3(u, w, nn);
            sub3(T.pt(t, f), P + (size_t)a * 3, o);
            if (dot3(nn, o) > 0) std::swap(b, c);
            if (cfg.long_edge > 0) {
                const double lim = cfg.long_edge * std::max({spacing[a], spacing[b], spacing[c]});
                double ab[3], bc[3], ca[3];
                sub3(P + (size_t)b * 3, P + (size_t)a * 3, ab);
                sub3(P + (size_t)c * 3, P + (size_t)b * 3, bc);
                sub3(P + (size_t)a * 3, P + (size_t)c * 3, ca);
                const double m2 = std::max({dot3(ab, ab), dot3(bc, bc), dot3(ca, ca)});
                if (m2 > lim * lim) { ++stats.long_faces; continue; }
            }
            mesh.F.push_back({vid(a), vid(b), vid(c)});
        }
    }

    stage("cleanup");
    dedup_faces(mesh, cfg.verbose);
    split_nonmanifold_vertices(mesh, cfg.verbose);
    remove_floaters(mesh, cfg.floater_min_faces, cfg.verbose);
    fill_holes(mesh, cfg.fill_hole_ratio, cfg.fill_hole_max_edges, cfg.verbose);
    split_nonmanifold_vertices(mesh, cfg.verbose);
    fix_degenerate_faces(mesh, cfg.degenerate_angle_deg, cfg.verbose);
    dedup_faces(mesh, cfg.verbose);
    if (cfg.quality_iters > 0) {
        improve_mesh_quality(mesh, cfg.quality_iters, cfg.verbose, cfg.threads);
        fix_degenerate_faces(mesh, cfg.degenerate_angle_deg, cfg.verbose);
    }
    remove_floaters(mesh, cfg.floater_min_faces, cfg.verbose);
    orient_mesh(mesh);
    if (mesh.C.size() != mesh.V.size()) mesh.C.clear();
    stats.faces = (int64_t)mesh.F.size();
    return mesh;
}

void smooth_points(std::vector<double>& xyz, const GraphCutConfig& cfg) {
    const int64_t n = (int64_t)xyz.size() / 3;
    const int K = cfg.smooth_k;
    if (K < 3 || n <= K) return;
    std::vector<double> out(xyz.size());
    std::vector<float> fpts((size_t)n * 3);
    for (int it = 0; it < cfg.smooth_iters; ++it) {
        const double* P = xyz.data();
        for (int64_t i = 0; i < n * 3; ++i) fpts[(size_t)i] = (float)(P[i] - P[i % 3]);
        const knn::KdTree3 tree(fpts.data(), n);
        parallel_for(n, cfg.threads, [&](int64_t b, int64_t e) {
            std::vector<float> d2((size_t)K);
            std::vector<int32_t> idx((size_t)K);
            std::vector<double> w((size_t)K);
            for (int64_t i = b; i < e; ++i) {
                const double* q = P + i * 3;
                const int got = tree.query(&fpts[(size_t)i * 3], -1, K, d2.data(), idx.data());
                for (int k = 0; k < 3; ++k) out[(size_t)i * 3 + k] = q[k];
                if (got < 3) continue;
                // Gaussian in the neighbourhood radius, as wide as the k-th neighbour.
                const double h2 = std::max((double)d2[(size_t)got - 1], 1e-24);
                double ws = 0, c[3] = {0, 0, 0};
                for (int j = 0; j < got; ++j) {
                    w[(size_t)j] = std::exp(-2.0 * d2[(size_t)j] / h2);
                    ws += w[(size_t)j];
                    for (int k = 0; k < 3; ++k) c[k] += w[(size_t)j] * P[(size_t)idx[(size_t)j] * 3 + k];
                }
                for (double& v : c) v /= ws;
                double C[9] = {};
                for (int j = 0; j < got; ++j) {
                    double d[3];
                    sub3(P + (size_t)idx[(size_t)j] * 3, c, d);
                    for (int r = 0; r < 3; ++r)
                        for (int cc = 0; cc < 3; ++cc) C[r * 3 + cc] += w[(size_t)j] * d[r] * d[cc];
                }
                double nrm[3];
                smallest_eigenvector(C, nrm);
                double d[3];
                sub3(q, c, d);
                const double off = d[0] * nrm[0] + d[1] * nrm[1] + d[2] * nrm[2];
                if (!std::isfinite(off)) continue;
                for (int k = 0; k < 3; ++k) out[(size_t)i * 3 + k] = q[k] - off * nrm[k];
            }
        });
        xyz.swap(out);
    }
}

void color_from_points(MeshData& mesh, const std::vector<double>& xyz,
                       const std::vector<uint8_t>& rgb, int threads) {
    const int64_t n = (int64_t)xyz.size() / 3;
    if (rgb.empty() || n == 0) return;
    std::vector<float> f((size_t)n * 3);
    for (int64_t i = 0; i < n * 3; ++i) f[(size_t)i] = (float)(xyz[(size_t)i] - xyz[(size_t)(i % 3)]);
    const knn::KdTree3 tree(f.data(), n);
    mesh.C.resize(mesh.V.size());
    parallel_for((int64_t)mesh.V.size(), threads, [&](int64_t b, int64_t e) {
        for (int64_t i = b; i < e; ++i) {
            const float q[3] = {(float)(mesh.V[(size_t)i][0] - xyz[0]), (float)(mesh.V[(size_t)i][1] - xyz[1]),
                                (float)(mesh.V[(size_t)i][2] - xyz[2])};
            float d2;
            int32_t j = 0;
            if (tree.query(q, -1, 1, &d2, &j))
                for (int k = 0; k < 3; ++k) mesh.C[(size_t)i][(size_t)k] = rgb[(size_t)j * 3 + k];
        }
    });
}

}  // namespace meshing
