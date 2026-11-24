// normal_refine_test -- mesh/NormalRefine.h on a cube whose vertices were
// pushed off the surface by noise, guided by exact normal maps rendered from
// synthetic cameras and written as the 8-bit PNGs `spirula geometry` writes.
// The noise RMS must drop at least 5x, the 90-degree creases must stay within
// 2 degrees, and no vertex may move further than the cap.

#include "mesh/NormalRefine.h"
#include "mesh/tests/SyntheticViews.h"

#include "external/stb_image_write.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_failures++;
}

// A closed cube, n x n cells per side, wound outward.
meshing::MeshData cube_mesh(int n) {
    meshing::MeshData m;
    std::map<std::tuple<int, int, int>, int> index;
    auto vertex = [&](const int g[3]) {
        auto key = std::make_tuple(g[0], g[1], g[2]);
        auto it = index.find(key);
        if (it != index.end()) return it->second;
        const int id = (int)m.V.size();
        m.V.push_back({-1 + 2.0f * g[0] / n, -1 + 2.0f * g[1] / n, -1 + 2.0f * g[2] / n});
        index.emplace(key, id);
        return id;
    };
    for (int axis = 0; axis < 3; ++axis)
        for (int side = 0; side <= 1; ++side)
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j) {
                    int q[4];
                    const int di[4] = {0, 1, 1, 0}, dj[4] = {0, 0, 1, 1};
                    for (int c = 0; c < 4; ++c) {
                        int g[3];
                        g[axis] = side * n;
                        g[(axis + 1) % 3] = i + di[c];
                        g[(axis + 2) % 3] = j + dj[c];
                        q[c] = vertex(g);
                    }
                    for (int t = 0; t < 2; ++t) {
                        std::array<int, 3> f = t == 0 ? std::array<int, 3>{q[0], q[1], q[2]} : std::array<int, 3>{q[0], q[2], q[3]};
                        const auto &a = m.V[(size_t)f[0]], &b = m.V[(size_t)f[1]], &c = m.V[(size_t)f[2]];
                        const double u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                        const double nn = u[(axis + 1) % 3] * v[(axis + 2) % 3] - u[(axis + 2) % 3] * v[(axis + 1) % 3];
                        if ((nn > 0) != (side == 1)) std::swap(f[1], f[2]);
                        m.F.push_back(f);
                    }
                }
    return m;
}

double rms_to_cube(const meshing::MeshData& m) {
    double s = 0;
    for (const auto& v : m.V) {
        const double p[3] = {v[0], v[1], v[2]};
        const double d = synth::cube_distance(p);
        s += d * d;
    }
    return std::sqrt(s / m.V.size());
}

void face_normal(const meshing::MeshData& m, int f, double n[3]) {
    const auto &a = m.V[(size_t)m.F[(size_t)f][0]], &b = m.V[(size_t)m.F[(size_t)f][1]], &c = m.V[(size_t)m.F[(size_t)f][2]];
    const double u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    n[0] = u[1] * v[2] - u[2] * v[1]; n[1] = u[2] * v[0] - u[0] * v[2]; n[2] = u[0] * v[1] - u[1] * v[0];
    const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (int k = 0; k < 3; ++k) n[k] /= l;
}

// Mean |dihedral - 90| over the mesh edges that lie on a cube edge (the clean
// mesh decides which), in degrees; `worst` gets the largest.
double crease_error(const meshing::MeshData& clean, const meshing::MeshData& m, double& worst) {
    std::map<std::pair<int, int>, std::vector<int>> faces;
    for (size_t f = 0; f < clean.F.size(); ++f)
        for (int k = 0; k < 3; ++k) {
            int a = clean.F[f][(size_t)k], b = clean.F[f][(size_t)((k + 1) % 3)];
            if (a > b) std::swap(a, b);
            faces[{a, b}].push_back((int)f);
        }
    auto on_edge = [&](int v) {
        int ones = 0;
        for (int k = 0; k < 3; ++k) ones += std::fabs(std::fabs(clean.V[(size_t)v][(size_t)k]) - 1) < 1e-6;
        return ones >= 2;
    };
    double sum = 0;
    int count = 0;
    worst = 0;
    for (const auto& [e, fs] : faces) {
        if (fs.size() != 2 || !on_edge(e.first) || !on_edge(e.second)) continue;
        double n0[3], n1[3], c0[3], c1[3];
        face_normal(clean, fs[0], c0);
        face_normal(clean, fs[1], c1);
        if (std::fabs(c0[0] * c1[0] + c0[1] * c1[1] + c0[2] * c1[2]) > 0.5) continue;   // same side
        face_normal(m, fs[0], n0);
        face_normal(m, fs[1], n1);
        const double d = std::max(-1.0, std::min(1.0, n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2]));
        const double err = std::fabs(std::acos(d) * 180 / 3.14159265358979323846 - 90);
        sum += err;
        worst = std::max(worst, err);
        ++count;
    }
    return count ? sum / count : 1e9;
}

void write_normal_maps(meshing::ViewSet& views, const fs::path& dir) {
    fs::create_directories(dir);
    const int W = 320, H = 240;   // half the camera record: the map is scaled to it
    std::vector<unsigned char> img((size_t)W * H * 3);
    for (size_t i = 0; i < views.views.size(); ++i) {
        meshing::MeshView& v = views.views[i];
        std::fill(img.begin(), img.end(), 0);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const double px[2] = {(x + 0.5) * v.cam.width / W, (y + 0.5) * v.cam.height / H};
                double d[3], t, n[3];
                if (!meshing::pixel_ray_file(v, px, d) || !synth::cube_hit(v.C, d, t, n)) continue;
                for (int r = 0; r < 3; ++r) {
                    const double c = v.R[r * 3] * n[0] + v.R[r * 3 + 1] * n[1] + v.R[r * 3 + 2] * n[2];
                    img[((size_t)y * W + x) * 3 + r] = (unsigned char)std::lround(std::min(255.0, std::max(0.0, 127.5 + 127.5 * c)));
                }
            }
        v.normal = (dir / ("n" + std::to_string(i) + ".png")).string();
        stbi_write_png(v.normal.c_str(), W, H, 3, img.data(), W * 3);
    }
}

}  // namespace

int main() {
    const fs::path tmp = fs::temp_directory_path() / "spirula_normal_refine_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    try {
        const int n = 24;
        const double h = 2.0 / n, sigma = 0.15 * h;
        const meshing::MeshData clean = cube_mesh(n);
        meshing::MeshData mesh = clean;
        // Noise along each vertex's (area-weighted) normal.
        std::vector<double> vn(clean.V.size() * 3, 0.0);
        for (size_t f = 0; f < clean.F.size(); ++f) {
            double nf[3];
            face_normal(clean, (int)f, nf);
            for (int k = 0; k < 3; ++k)
                for (int j = 0; j < 3; ++j) vn[(size_t)clean.F[f][(size_t)k] * 3 + j] += nf[j];
        }
        std::mt19937 rng(11);
        std::normal_distribution<double> noise(0, sigma);
        for (size_t i = 0; i < mesh.V.size(); ++i) {
            double* d = &vn[i * 3];
            const double l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            const double e = std::max(-3 * sigma, std::min(3 * sigma, noise(rng)));
            for (int k = 0; k < 3; ++k) mesh.V[i][(size_t)k] += (float)(e * d[k] / l);
        }
        const meshing::MeshData noisy = mesh;
        const double before = rms_to_cube(mesh);

        meshing::ViewSet views = synth::ring(30, 4.5);
        write_normal_maps(views, tmp);
        meshing::RefineConfig cfg;
        cfg.max_shift = 4 * sigma;
        meshing::RefineStats st;
        std::vector<float> shift;
        meshing::refine_with_normals(mesh, views, {}, cfg, st, shift);
        const double after = rms_to_cube(mesh);
        std::printf("     views %d, guided %lld of %zu, mean confidence %.3f\n", st.views, (long long)st.guided,
                    mesh.F.size(), st.mean_conf);
        std::printf("     noise RMS %.5f -> %.5f (%.1fx); shift mean %.5f max %.5f cap %.5f\n", before, after,
                    before / std::max(after, 1e-30), st.mean_shift, st.max_shift, st.cap);
        check(st.views == 30 && st.guided > (int64_t)(0.95 * mesh.F.size()), "guides: nearly every face has one");
        check(after * 5 <= before, "noise RMS drops at least 5x");
        double worst_before = 0, worst = 0;
        const double crease_before = crease_error(clean, noisy, worst_before);
        const double crease = crease_error(clean, mesh, worst);
        std::printf("     crease error %.2f deg (worst %.2f), was %.2f (worst %.2f)\n", crease, worst, crease_before, worst_before);
        check(crease < 2.0, "creases stay within 2 degrees of 90");
        double max_move = 0;
        for (size_t i = 0; i < mesh.V.size(); ++i) {
            double d2 = 0;
            for (int k = 0; k < 3; ++k) d2 += std::pow((double)mesh.V[i][(size_t)k] - noisy.V[i][(size_t)k], 2);
            max_move = std::max(max_move, std::sqrt(d2));
        }
        check(max_move <= cfg.max_shift * (1 + 1e-5) + 1e-6, "no vertex moves past --max-shift");
        check(shift.size() == mesh.V.size(), "a shift per vertex");
    } catch (const std::exception& e) {
        check(false, std::string("exception: ") + e.what());
    }
    fs::remove_all(tmp, ec);
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
