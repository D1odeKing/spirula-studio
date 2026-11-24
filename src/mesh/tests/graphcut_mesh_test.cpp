// graphcut_mesh_test -- mesh/GraphCutMesher.h on points sampled from a sphere
// and a cube, seen by synthetic cameras all around: the cut must give one
// watertight piece whose Chamfer distance to the true surface is under half
// the point spacing. Samples are random and jittered by 2% of the spacing, as a
// dense cloud is: exactly cospherical or coplanar input is the Delaunay's
// degenerate case (docs/notes/delaunay-degeneracy.md), not this mesher's.

#include "mesh/GraphCutMesher.h"
#include "mesh/MeshDistance.h"
#include "mesh/tests/SyntheticViews.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_failures++;
}

// Every edge on exactly two faces, traversed once each way.
bool watertight(const meshing::MeshData& m) {
    std::map<std::pair<int, int>, int> dir;
    for (const auto& f : m.F)
        for (int k = 0; k < 3; ++k) dir[{f[(size_t)k], f[(size_t)((k + 1) % 3)]}]++;
    long open = 0, multi = 0, flipped = 0;
    for (const auto& [e, c] : dir) {
        auto it = dir.find({e.second, e.first});
        if (c > 1) ++multi;
        else if (it == dir.end()) ++open;
        else if (it->second != 1) ++flipped;
    }
    std::printf("     edges: open %ld, repeated %ld, unmatched %ld\n", open, multi, flipped);
    return !m.F.empty() && open == 0 && multi == 0 && flipped == 0;
}

int components(const meshing::MeshData& m) {
    std::vector<int> parent(m.V.size());
    for (size_t i = 0; i < parent.size(); ++i) parent[i] = (int)i;
    auto find = [&](int x) { while (parent[(size_t)x] != x) x = parent[(size_t)x] = parent[(size_t)parent[(size_t)x]]; return x; };
    for (const auto& f : m.F) {
        parent[(size_t)find(f[1])] = find(f[0]);
        parent[(size_t)find(f[2])] = find(f[0]);
    }
    std::vector<char> used(m.V.size(), 0);
    for (const auto& f : m.F) for (int v : f) used[(size_t)v] = 1;
    int n = 0;
    for (size_t i = 0; i < used.size(); ++i) n += used[i] && find((int)i) == (int)i;
    return n;
}

// Mean distance both ways between the mesh and the true surface.
template <typename SurfaceDistance>
double chamfer(const meshing::MeshData& mesh, const std::vector<double>& truth_samples, SurfaceDistance&& to_truth) {
    std::vector<double> s;
    meshing::sample_surface(mesh, 50000, 3, s);
    double acc = 0;
    for (size_t i = 0; i < s.size() / 3; ++i) acc += to_truth(&s[i * 3]);
    acc /= (double)(s.size() / 3);
    const meshing::TriangleBvh bvh(mesh);
    double comp = 0;
    for (size_t i = 0; i < truth_samples.size() / 3; ++i) comp += std::sqrt(bvh.closest(&truth_samples[i * 3]));
    comp /= (double)(truth_samples.size() / 3);
    std::printf("     accuracy %.5f, completeness %.5f\n", acc, comp);
    return 0.5 * (acc + comp);
}

void report(const char* tag, const meshing::MeshData& m, const meshing::GraphCutStats& st) {
    std::printf("     %s: points %lld, rays %lld, tets %lld, inside %lld, relabelled %lld, faces %lld, long %lld\n", tag,
                (long long)st.points, (long long)st.rays, (long long)st.tets, (long long)st.inside,
                (long long)st.relabelled, (long long)m.F.size(), (long long)st.long_faces);
}

void sphere() {
    const int N = 20000;
    const double spacing = std::sqrt(4 * 3.14159265358979323846 / N);
    std::mt19937 rng(5);
    std::normal_distribution<double> g(0, 1);
    std::uniform_real_distribution<double> jit(-0.02 * spacing, 0.02 * spacing);
    std::vector<double> pts;
    for (int i = 0; i < N; ++i) {
        double p[3] = {g(rng), g(rng), g(rng)};
        const double l = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        const double r = 1 + jit(rng);
        pts.insert(pts.end(), {r * p[0] / l, r * p[1] / l, r * p[2] / l});
    }
    const double golden = 3.14159265358979323846 * (3 - std::sqrt(5.0));
    meshing::GraphCutConfig cfg;
    cfg.verbose = false;
    cfg.floater_min_faces = 50;
    meshing::GraphCutStats st;
    const meshing::MeshData m = meshing::graph_cut_mesh(pts, {}, synth::ring(40, 3.5), cfg, st);
    report("sphere", m, st);
    check(watertight(m), "sphere: watertight");
    check(components(m) == 1, "sphere: one piece, no floaters");
    std::vector<double> truth;
    for (int i = 0; i < 30000; ++i) {
        const double z = 1 - 2 * (i + 0.37) / 30000, r = std::sqrt(1 - z * z), a = golden * i * 1.0001;
        truth.insert(truth.end(), {r * std::cos(a), r * std::sin(a), z});
    }
    const double c = chamfer(m, truth, [](const double* p) {
        return std::fabs(std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) - 1);
    });
    check(c < 0.5 * spacing, "sphere: Chamfer " + std::to_string(c) + " < half the spacing " + std::to_string(0.5 * spacing));
}

void cube() {
    const int per_face = 1600;
    const double h = std::sqrt(4.0 / per_face);
    std::mt19937 rng(9);
    std::uniform_real_distribution<double> u(-1, 1), jit(-0.02 * h, 0.02 * h);
    std::vector<double> pts;
    for (int axis = 0; axis < 3; ++axis)
        for (int side = -1; side <= 1; side += 2)
            for (int i = 0; i < per_face; ++i) {
                double p[3];
                p[axis] = side * (1 + jit(rng));
                p[(axis + 1) % 3] = u(rng);
                p[(axis + 2) % 3] = u(rng);
                pts.insert(pts.end(), {p[0], p[1], p[2]});
            }
    meshing::GraphCutConfig cfg;
    cfg.verbose = false;
    cfg.floater_min_faces = 50;
    meshing::GraphCutStats st;
    const meshing::MeshData m = meshing::graph_cut_mesh(pts, {}, synth::ring(40, 4.5), cfg, st);
    report("cube", m, st);
    check(watertight(m), "cube: watertight");
    check(components(m) == 1, "cube: one piece, no floaters");
    std::vector<double> truth;
    for (int axis = 0; axis < 3; ++axis)
        for (int side = -1; side <= 1; side += 2)
            for (int i = 0; i < 70; ++i)
                for (int j = 0; j < 70; ++j) {
                    double p[3];
                    p[axis] = side;
                    p[(axis + 1) % 3] = -1 + (i + 0.5) * 2.0 / 70;
                    p[(axis + 2) % 3] = -1 + (j + 0.5) * 2.0 / 70;
                    truth.insert(truth.end(), {p[0], p[1], p[2]});
                }
    const double c = chamfer(m, truth, [](const double* p) { return synth::cube_distance(p); });
    check(c < 0.5 * h, "cube: Chamfer " + std::to_string(c) + " < half the spacing " + std::to_string(0.5 * h));
}

}  // namespace

int main() {
    try {
        sphere();
        cube();
    } catch (const std::exception& e) {
        check(false, std::string("exception: ") + e.what());
    }
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
