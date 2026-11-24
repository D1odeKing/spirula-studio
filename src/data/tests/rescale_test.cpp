// rescale_test -- `spirula scale`'s core (data/DatasetRescale.h) on small
// datasets written here: after p -> s p in the FILE frame, the cameras, the
// sparse and dense points, a splat model in its run's training frame, a mesh
// and gauge.txt must all agree, and every distance must be s times what it was.
// A transforms.json with applied_transform is the case where raw != file.

#include "checkpoint/SplatPly.h"
#include "core/Sha256.h"
#include "data/DatasetParser.h"
#include "data/DatasetRescale.h"
#include "data/SceneTransform.h"
#include "data/SparseEdit.h"
#include "mesh/MeshExport.h"
#include "mesh/MeshImport.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_failures++;
}

struct Cam { double q[4], t[3]; std::string name; };

void quat_to_R(const double q[4], double R[9]) {
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    const double M[9] = {1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w),
                         2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w),
                         2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)};
    for (int i = 0; i < 9; i++) R[i] = M[i];
}

std::vector<Cam> cameras() {
    std::vector<Cam> c(3);
    const double qs[3][4] = {{1, 0, 0, 0}, {0.9238795, 0, 0.3826834, 0}, {0.9238795, 0.3826834, 0, 0}};
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 4; k++) c[i].q[k] = qs[i][k];
        c[i].t[0] = 0.3 * i; c[i].t[1] = -0.2 * i; c[i].t[2] = 4.0 + i;
        c[i].name = "img" + std::to_string(i) + ".png";
    }
    return c;
}

std::vector<double> points() {
    std::vector<double> p;
    for (int i = 0; i < 10; i++) {
        p.push_back(std::sin(1.3 * i));
        p.push_back(std::cos(0.7 * i));
        p.push_back(0.1 * i - 0.5);
    }
    return p;
}

void write_colmap(const fs::path& model, const std::vector<Cam>& cams, const std::vector<double>& pts) {
    fs::create_directories(model);
    std::ofstream c(model / "cameras.txt");
    c << "# cameras\n1 PINHOLE 640 480 500 500 320 240\n";
    std::ofstream im(model / "images.txt");
    im << "# images\n";
    for (size_t i = 0; i < cams.size(); i++) {
        char buf[512];
        std::snprintf(buf, sizeof buf, "%zu %.17g %.17g %.17g %.17g %.17g %.17g %.17g 1 %s\n", i + 1, cams[i].q[0],
                      cams[i].q[1], cams[i].q[2], cams[i].q[3], cams[i].t[0], cams[i].t[1], cams[i].t[2],
                      cams[i].name.c_str());
        im << buf << "10.5 20.5 " << (i + 1) << "\n";
    }
    std::ofstream p(model / "points3D.txt");
    p << "# points\n";
    for (size_t i = 0; i < pts.size() / 3; i++) {
        char buf[256];
        std::snprintf(buf, sizeof buf, "%zu %.17g %.17g %.17g 10 20 30 0.5 1 0 2 0\n", i + 1, pts[i * 3],
                      pts[i * 3 + 1], pts[i * 3 + 2]);
        p << buf;
    }
    std::ofstream g(model / "gauge.txt");
    g << "# What this model's frame means, from spirula sfm.\noriented 1\nmetric 0\nup sensors\nscale 1\n";
}

void write_nerfstudio(const fs::path& dir, const std::vector<Cam>& cams, const std::vector<double>& pts,
                      const double A[12]) {
    fs::create_directories(dir);
    std::ofstream j(dir / "transforms.json");
    j.precision(17);
    j << "{\n \"camera_model\": \"PINHOLE\", \"w\": 640, \"h\": 480,\n"
         " \"fl_x\": 500, \"fl_y\": 500, \"cx\": 320, \"cy\": 240,\n"
         " \"applied_transform\": [";
    for (int r = 0; r < 3; r++) {
        j << (r ? ", [" : "[");
        for (int c = 0; c < 4; c++) j << (c ? ", " : "") << A[r * 4 + c];
        j << "]";
    }
    j << "],\n \"ply_file_path\": \"cloud.ply\",\n \"frames\": [\n";
    for (size_t i = 0; i < cams.size(); i++) {
        double R[9];
        quat_to_R(cams[i].q, R);
        double c2w[12];
        for (int r = 0; r < 3; r++) {
            c2w[r * 4 + 0] = R[0 * 3 + r];
            c2w[r * 4 + 1] = -R[1 * 3 + r];
            c2w[r * 4 + 2] = -R[2 * 3 + r];
            c2w[r * 4 + 3] = -(R[0 * 3 + r] * cams[i].t[0] + R[1 * 3 + r] * cams[i].t[1] + R[2 * 3 + r] * cams[i].t[2]);
        }
        double m[12];
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 4; c++)
                m[r * 4 + c] = A[r * 4 + 0] * c2w[0 * 4 + c] + A[r * 4 + 1] * c2w[1 * 4 + c] +
                               A[r * 4 + 2] * c2w[2 * 4 + c] + (c == 3 ? A[r * 4 + 3] : 0.0);
        j << "  {\"file_path\": \"images/" << cams[i].name << "\", \"transform_matrix\": [";
        for (int r = 0; r < 3; r++)
            j << "[" << m[r * 4] << ", " << m[r * 4 + 1] << ", " << m[r * 4 + 2] << ", " << m[r * 4 + 3] << "], ";
        j << "[0, 0, 0, 1]]}" << (i + 1 < cams.size() ? ",\n" : "\n");
    }
    j << " ]\n}\n";
    std::vector<double> q(pts.size());
    for (size_t i = 0; i < pts.size() / 3; i++)
        for (int r = 0; r < 3; r++)
            q[i * 3 + r] = A[r * 4 + 0] * pts[i * 3] + A[r * 4 + 1] * pts[i * 3 + 1] + A[r * 4 + 2] * pts[i * 3 + 2] +
                           A[r * 4 + 3];
    spirula::write_ply_points((dir / "cloud.ply").string(), q.data(), nullptr, (int64_t)q.size() / 3, nullptr);
}

ParsedDataset parse(const fs::path& dir) {
    DatasetParserConfig cfg;
    cfg.require_image_files = false;
    return parse_dataset(dir.string(), cfg, "");
}

// Camera centres in the FILE frame.
std::vector<double> centres_file(const ParsedDataset& ds) {
    std::vector<double> out;
    for (int64_t i = 0; i < ds.num_cameras; i++) {
        const float* m = &ds.c2w[(size_t)i * 12];
        const double raw[3] = {m[3] + ds.center[0], m[7] + ds.center[1], m[11] + ds.center[2]};
        for (int r = 0; r < 3; r++) {
            double v = ds.raw_to_file[r * 4 + 3];
            for (int k = 0; k < 3; k++) v += ds.raw_to_file[r * 4 + k] * raw[k];
            out.push_back(v);
        }
    }
    return out;
}

double worst_ratio_error(const std::vector<double>& before, const std::vector<double>& after, double s) {
    double worst = before.size() == after.size() ? 0.0 : 1e30;
    for (size_t i = 0; i < before.size() && i < after.size(); i++)
        worst = std::max(worst, std::fabs(after[i] - s * before[i]));
    return worst;
}

double dist(const double* a, const double* b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

void test_fit() {
    std::vector<spirula::ScalePair> pairs(3);
    const double s = 2.5;
    for (int i = 0; i < 3; i++) {
        pairs[i].a[0] = i; pairs[i].b[0] = i + 1.0 + i; pairs[i].b[1] = 0.5 * i;
        pairs[i].truth = s * dist(pairs[i].a, pairs[i].b);
    }
    const spirula::ScaleFit f = spirula::fit_scale(pairs);
    check(std::fabs(f.s - s) < 1e-12, "fit: exact pairs give the exact scale");
    check(f.spread < 1e-12 && f.sigma < 1e-12, "fit: exact pairs have no spread");
    pairs[0].truth *= 1.01;
    const spirula::ScaleFit g = spirula::fit_scale(pairs);
    check(g.spread > 1e-3 && g.sigma > 0 && std::fabs(g.residual[0]) > 0, "fit: a 1% error shows as spread");
}

void test_colmap(const fs::path& root) {
    const fs::path dir = root / "colmap";
    const auto cams = cameras();
    const auto pts = points();
    write_colmap(dir / "sparse" / "0", cams, pts);

    // A dense cloud with the manifest commit_edit re-signs.
    fs::create_directories(dir / "dense");
    spirula::write_ply_points((dir / "dense" / "roma.ply").string(), pts.data(), nullptr, (int64_t)pts.size() / 3,
                              nullptr, nullptr, spirula::PlyCoordinates::Float64);
    std::ofstream(dir / "dense" / "manifest.json")
        << "{\"version\": 2, \"complete\": true, \"cloud_sha256\": \""
        << spirula::sha256_file((dir / "dense" / "roma.ply").string()) << "\", \"statistics\": {\"exported\": 10}}\n";

    // A run trained in p_train = k (p_raw - c).
    const double k = 0.5, c[3] = {1, 2, 3};
    const fs::path run = dir / "outputs" / "run";
    fs::create_directories(run / "step-000100");
    spirula::SceneTransform T;
    T.scale = k;
    for (int i = 0; i < 3; i++) T.t[i] = -k * c[i];
    std::ofstream(run / "scene_transform.json") << spirula::scene_transform_json(T, "none", c);
    spirula::SplatCloud sc;
    sc.num = 3;
    for (int i = 0; i < 3; i++) {
        for (int r = 0; r < 3; r++) sc.means.push_back((float)(k * (pts[i * 3 + r] - c[r])));
        sc.quats.insert(sc.quats.end(), {1, 0, 0, 0});
        sc.scales.insert(sc.scales.end(), {-3, -3, -3});
        sc.opacities.push_back(0.5f);
        sc.features_dc.insert(sc.features_dc.end(), {0.1f, 0.2f, 0.3f});
    }
    spirula::write_splat_ply(sc, (run / "step-000100" / "splat.ply").string());

    meshing::MeshData mesh;
    for (int i = 0; i < 3; i++) mesh.V.push_back({(float)pts[i * 3], (float)pts[i * 3 + 1], (float)pts[i * 3 + 2]});
    mesh.F.push_back({0, 1, 2});
    meshing::write_mesh(mesh, meshing::MeshColorMode::None, meshing::parse_one_mesh_format("ply"),
                        (dir / "m").string(), false);

    const ParsedDataset before = parse(dir);
    const double s = 3.25;
    spirula::RescaleOptions opt;
    opt.apply = false;
    opt.splats.push_back(run.string());
    opt.meshes.push_back((dir / "m.ply").string());
    spirula::RescaleReport dry;
    spirula::rescale_dataset(dir.string(), s, 0.004, opt, dry);
    const ParsedDataset same = parse(dir);
    check(worst_ratio_error(centres_file(before), centres_file(same), 1.0) < 1e-9 && !same.gauge_metric,
          "colmap: a dry run writes nothing");
    check(dry.rescaled.size() >= 5, "colmap: the dry run lists sparse, gauge, dense, splat and mesh");

    opt.apply = true;
    spirula::RescaleReport rep;
    spirula::rescale_dataset(dir.string(), s, 0.004, opt, rep);
    const ParsedDataset after = parse(dir);
    check(worst_ratio_error(centres_file(before), centres_file(after), s) < 1e-5, "colmap: camera centres x s");
    check(worst_ratio_error(before.points.xyz, after.points.xyz, s) < 1e-9, "colmap: sparse points x s");
    check(after.gauge_metric && after.gauge_oriented, "colmap: gauge.txt says metric and keeps oriented");
    {
        std::ifstream g(dir / "sparse" / "0" / "gauge.txt");
        std::string text((std::istreambuf_iterator<char>(g)), std::istreambuf_iterator<char>());
        check(text.find("scale measured") != std::string::npos && text.find("scale_sigma 0.004") != std::string::npos &&
                  text.find("up sensors") != std::string::npos,
              "colmap: gauge.txt records the measured scale and its sigma");
    }
    // The pixel a point lands on is a similarity invariant: check it survived.
    {
        double worst = 0;
        for (int64_t i = 0; i < after.num_cameras; i++)
            for (int64_t p = 0; p < after.points.num(); p++) {
                auto uv = [&](const ParsedDataset& ds, double o[2]) {
                    const float* m = &ds.c2w[(size_t)i * 12];
                    double d[3], cc[3];
                    for (int r = 0; r < 3; r++) d[r] = ds.points.xyz[(size_t)p * 3 + r] - m[r * 4 + 3];
                    for (int q = 0; q < 3; q++) cc[q] = m[0 * 4 + q] * d[0] + m[1 * 4 + q] * d[1] + m[2 * 4 + q] * d[2];
                    o[0] = 500 * cc[0] / -cc[2];
                    o[1] = 500 * cc[1] / -cc[2];
                };
                double a[2], b[2];
                uv(before, a);
                uv(after, b);
                worst = std::max({worst, std::fabs(a[0] - b[0]), std::fabs(a[1] - b[1])});
            }
        check(worst < 1e-3, "colmap: every point projects to the same pixel");
    }
    const ColmapPoints3D dense = read_ply_points((dir / "dense" / "roma.ply").string());
    check(worst_ratio_error(pts, dense.xyz, s) < 1e-9, "colmap: dense cloud x s");
    check(fs::exists(dir / "dense" / "roma.original.ply"), "colmap: the original dense cloud is kept");

    const spirula::SplatCloud sa = spirula::read_splat_ply((run / "step-000100" / "splat.ply").string());
    double worst = 0;
    for (int i = 0; i < 3; i++)
        for (int r = 0; r < 3; r++) {
            const double raw = sa.means[(size_t)i * 3 + r] / k + c[r];
            worst = std::max(worst, std::fabs(raw - s * pts[(size_t)i * 3 + r]));
        }
    check(worst < 1e-5, "colmap: splats, read back through the run's frame, x s");
    check(std::fabs(sa.scales[0] - (-3 + std::log(s))) < 1e-5, "colmap: splat log-scales grow by ln s");

    meshing::MeshData m2;
    std::string err;
    check(meshing::read_mesh((dir / "m.ply").string(), m2, err) && m2.V.size() == 3 &&
              std::fabs(m2.V[1][0] - s * pts[3]) < 1e-5,
          "colmap: mesh vertices x s");
    const double d0 = dist(&before.points.xyz[0], &before.points.xyz[3]);
    const double d1 = dist(&after.points.xyz[0], &after.points.xyz[3]);
    check(std::fabs(d1 - s * d0) < 1e-9, "colmap: a distance is multiplied by s");
}

void test_nerfstudio(const fs::path& root) {
    const fs::path dir = root / "nerfstudio";
    // file = A raw: a turn about z and a shift, so raw != file.
    const double A[12] = {0, -1, 0, 0.5, 1, 0, 0, -0.25, 0, 0, 1, 2.0};
    const auto pts = points();
    write_nerfstudio(dir, cameras(), pts, A);
    const ParsedDataset before = parse(dir);
    const ColmapPoints3D file_before = read_ply_points((dir / "cloud.ply").string());
    const double s = 0.4;
    spirula::RescaleOptions opt;
    opt.apply = true;
    spirula::RescaleReport rep;
    spirula::rescale_dataset(dir.string(), s, 0, opt, rep);
    const ParsedDataset after = parse(dir);
    const ColmapPoints3D file_after = read_ply_points((dir / "cloud.ply").string());
    check(worst_ratio_error(file_before.xyz, file_after.xyz, s) < 1e-6, "nerfstudio: the cloud file x s about the file origin");
    check(worst_ratio_error(centres_file(before), centres_file(after), s) < 1e-5, "nerfstudio: camera centres (file frame) x s");
    check(after.gauge_metric, "nerfstudio: gauge.txt beside transforms.json says metric");
}

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "spirula_rescale_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    try {
        test_fit();
        test_colmap(root);
        test_nerfstudio(root);
    } catch (const std::exception& e) {
        check(false, std::string("exception: ") + e.what());
    }
    fs::remove_all(root, ec);
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
