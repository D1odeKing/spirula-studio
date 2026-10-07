// random_points_test -- the random seed cloud (data/RandomPoints.h) against
// cameras whose spread is known: each shape's sample covariance must be the
// one the cameras set, and the uniform shapes must stay inside their bounds.

#include "data/RandomPoints.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_failures++;
}

// c2w [N,3,4] with identity rotation at the given positions.
std::vector<float> cameras_at(const std::vector<std::array<double, 3>>& pos) {
    std::vector<float> c2w(pos.size() * 12, 0.0f);
    for (size_t i = 0; i < pos.size(); i++) {
        float* m = &c2w[i * 12];
        m[0] = m[5] = m[10] = 1.0f;
        for (int r = 0; r < 3; r++) m[r * 4 + 3] = (float)pos[i][r];
    }
    return c2w;
}

// Sample mean and covariance of an [N,3] cloud.
void moments(const ColmapPoints3D& p, double mean[3], double cov[9]) {
    const int64_t n = p.num();
    for (int r = 0; r < 3; r++) mean[r] = 0;
    for (int64_t i = 0; i < n; i++)
        for (int r = 0; r < 3; r++) mean[r] += p.xyz[i * 3 + r] / n;
    for (int k = 0; k < 9; k++) cov[k] = 0;
    for (int64_t i = 0; i < n; i++)
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                cov[r * 3 + c] += (p.xyz[i * 3 + r] - mean[r]) *
                                  (p.xyz[i * 3 + c] - mean[c]) / n;
}

}  // namespace

int main() {
    // Cameras on an axis-aligned Gaussian about (5, -2, 1), standard
    // deviations 3, 2 and 1 along x, y, z -- each drawn offset mirrored into
    // all eight octants, so the mean and the axes are exact, not noisy.
    const double centre[3] = {5.0, -2.0, 1.0}, sd[3] = {3.0, 2.0, 1.0};
    std::mt19937 rng(7);
    std::normal_distribution<double> g(0.0, 1.0);
    std::vector<std::array<double, 3>> pos;
    for (int k = 0; k < 500; k++) {
        const double o[3] = {sd[0] * g(rng), sd[1] * g(rng), sd[2] * g(rng)};
        for (int m = 0; m < 8; m++)
            pos.push_back({centre[0] + ((m & 1) ? -o[0] : o[0]),
                           centre[1] + ((m & 2) ? -o[1] : o[1]),
                           centre[2] + ((m & 4) ? -o[2] : o[2])});
    }
    const std::vector<float> c2w = cameras_at(pos);
    const int64_t n = (int64_t)pos.size();

    // What the cloud should copy: the cameras' own mean and covariance.
    double cam_mean[3] = {0, 0, 0}, cam_cov[9] = {0};
    for (const auto& q : pos)
        for (int r = 0; r < 3; r++) cam_mean[r] += q[r] / n;
    for (const auto& q : pos)
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                cam_cov[r * 3 + c] += (q[r] - cam_mean[r]) * (q[c] - cam_mean[c]) / n;

    for (const std::string dist : {"anisotropic-gaussian", "ellipsoid", "box"}) {
        RandomPointsConfig cfg;
        cfg.count = 200000;
        cfg.distribution = dist;
        cfg.center = "camera-mean";
        cfg.spread = "mean";
        cfg.std_scale = 0.5;
        RandomPointsFit fit;
        const ColmapPoints3D p = random_seed_points(c2w.data(), n, cfg, &fit);
        double mean[3], cov[9];
        moments(p, mean, cov);
        bool ok = p.num() == cfg.count && p.rgb.size() == p.xyz.size();
        for (int r = 0; r < 3; r++) {
            ok &= std::fabs(fit.center[r] - cam_mean[r]) < 1e-4;
            ok &= std::fabs(mean[r] - cam_mean[r]) < 0.01 * sd[0];
            ok &= std::fabs(fit.sigma[r] - 0.5 * std::sqrt(cam_cov[r * 4])) < 1e-4;
            for (int c = 0; c < 3; c++) {
                const double want = 0.25 * cam_cov[r * 3 + c];
                const double scale = 0.25 * std::sqrt(cam_cov[r * 4] * cam_cov[c * 4]);
                ok &= std::fabs(cov[r * 3 + c] - want) < 0.01 * scale;
            }
        }
        check(ok, dist + ": centre and covariance are the cameras'");
        if (dist == "anisotropic-gaussian") continue;
        // Inside a solid ellipsoid of semi-axes sigma * sqrt(5), or a box of
        // half-extent sigma * sqrt(3), along x, y, z.
        bool inside = true;
        for (int64_t i = 0; i < p.num(); i++) {
            double e = 0, b = 0;
            for (int r = 0; r < 3; r++) {
                const double u = (p.xyz[i * 3 + r] - fit.center[r]) / fit.sigma[r];
                e += u * u / 5.0;
                b = std::fmax(b, std::fabs(u) / std::sqrt(3.0));
            }
            inside &= (dist == "ellipsoid" ? e : b) <= 1.0 + 1e-6;
        }
        check(inside, dist + ": every point inside its bounds");
    }

    {
        RandomPointsConfig cfg;
        cfg.count = 100000;
        cfg.center = "origin";
        cfg.spread = "mean";
        RandomPointsFit fit;
        const ColmapPoints3D p = random_seed_points(c2w.data(), n, cfg, &fit);
        // Isotropic about the origin: a third of the mean squared distance
        // from it, which counts the cameras' offset as spread too.
        double ms = 0;
        for (const auto& q : pos) ms += (q[0] * q[0] + q[1] * q[1] + q[2] * q[2]) / n;
        const double want = std::sqrt(ms / 3.0);
        double mean[3], cov[9];
        moments(p, mean, cov);
        bool ok = fit.center == std::array<double, 3>{0, 0, 0};
        for (int r = 0; r < 3; r++) {
            ok &= std::fabs(fit.sigma[r] - want) < 1e-3 * want;
            ok &= std::fabs(std::sqrt(cov[r * 3 + r]) - want) < 0.02 * want;
            ok &= std::fabs(mean[r]) < 0.05 * want;
        }
        check(ok, "isotropic about the origin");
    }

    {
        // A tenth of the cameras far off: the median barely moves, the mean does.
        std::vector<std::array<double, 3>> shifted = pos;
        for (size_t i = 0; i < shifted.size() / 10; i++) shifted[i][0] += 1000.0;
        const std::vector<float> c = cameras_at(shifted);
        RandomPointsConfig cfg;
        cfg.count = 10;
        cfg.center = "camera-median";
        RandomPointsFit med, mean;
        random_seed_points(c.data(), n, cfg, &med);
        cfg.spread = "mean";
        random_seed_points(c.data(), n, cfg, &mean);
        const double clean = std::sqrt((sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]) / 3.0);
        check(med.sigma[0] < 1.5 * clean && mean.sigma[0] > 50.0 * clean,
              "median spread ignores outlying cameras, mean does not");
    }

    {
        // A ring of cameras looking at (1, 2, 3): the focus is that point.
        std::vector<float> ring;
        const double at[3] = {1.0, 2.0, 3.0};
        for (int i = 0; i < 36; i++) {
            const double a = i * 3.14159265358979 / 18.0;
            const double p[3] = {at[0] + 4 * std::cos(a), at[1] + 4 * std::sin(a), at[2] + 1.0};
            // OpenGL: the camera looks down -Z, so +Z points away from `at`.
            double z[3] = {p[0] - at[0], p[1] - at[1], p[2] - at[2]};
            const double zl = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
            for (double& v : z) v /= zl;
            double x[3] = {-z[1], z[0], 0.0};
            const double xl = std::sqrt(x[0] * x[0] + x[1] * x[1]);
            for (double& v : x) v /= xl;
            const double y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2],
                                 z[0] * x[1] - z[1] * x[0]};
            for (int r = 0; r < 3; r++) {
                ring.push_back((float)x[r]);
                ring.push_back((float)y[r]);
                ring.push_back((float)z[r]);
                ring.push_back((float)p[r]);
            }
        }
        RandomPointsConfig cfg;
        cfg.count = 10;
        cfg.center = "camera-focus";
        RandomPointsFit fit;
        random_seed_points(ring.data(), 36, cfg, &fit);
        bool ok = true;
        for (int r = 0; r < 3; r++) ok &= std::fabs(fit.center[r] - at[r]) < 1e-3;
        check(ok, "camera-focus centres the cloud where the cameras look");
    }

    {
        const std::vector<float> one = cameras_at({{{1.0, 2.0, 3.0}}});
        RandomPointsConfig cfg;
        cfg.count = 10;
        bool threw = false;
        try { random_seed_points(one.data(), 1, cfg); } catch (const std::exception&) { threw = true; }
        check(threw, "one camera has no spread to size the cloud by");
        cfg.distribution = "cube";
        threw = false;
        try { random_seed_points(c2w.data(), n, cfg); } catch (const std::exception&) { threw = true; }
        check(threw, "an unknown distribution is refused");
    }

    std::printf("\n%s\n", g_failures ? "FAILURES" : "all passed");
    return g_failures ? 1 : 0;
}
