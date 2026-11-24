#include "mesh/MeshViews.h"

#include "core/CameraModel.h"
#include "data/DatasetParser.h"
#include "data/SceneTransform.h"
#include "external/stb_image.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace meshing {

namespace fs = std::filesystem;

namespace {

double det3(const double A[9]) {
    return A[0] * (A[4] * A[8] - A[5] * A[7]) - A[1] * (A[3] * A[8] - A[5] * A[6]) +
           A[2] * (A[3] * A[7] - A[4] * A[6]);
}

}  // namespace

ViewSet load_views(const std::string& dataset, const ViewOptions& opt) {
    DatasetParserConfig cfg;
    cfg.recon_dir = opt.recon_dir;
    cfg.image_dir = opt.image_dir;
    cfg.normal_dir = opt.normal_dir;
    cfg.depth_dir = opt.depth_dir;
    cfg.require_image_files = false;
    cfg.eval_mode = "all";
    cfg.center_mode = "none";
    const ParsedDataset ds = parse_dataset(dataset, cfg, "");
    if (ds.num_cameras <= 0) throw std::runtime_error("dataset has no cameras: " + dataset);

    ViewSet out;
    out.raw_to_file = ds.raw_to_file;
    out.metric = ds.gauge_metric;
    // file = A raw + t with A = s Q: the cameras turn by Q, and depths shrink by s.
    double A[9], t[3];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) A[r * 3 + c] = ds.raw_to_file[r * 4 + c];
        t[r] = ds.raw_to_file[r * 4 + 3];
    }
    const double s = std::cbrt(std::fabs(det3(A)));
    if (!(s > 0)) throw std::runtime_error("dataset raw-to-file transform is singular");
    double Q[9];
    for (int i = 0; i < 9; ++i) Q[i] = A[i] / s;

    out.views.resize((size_t)ds.num_cameras);
    for (int64_t i = 0; i < ds.num_cameras; ++i) {
        MeshView& v = out.views[(size_t)i];
        v.image = ds.image_filenames[(size_t)i];
        if (!ds.normal_filenames.empty()) v.normal = ds.normal_filenames[(size_t)i];
        if (!ds.depth_filenames.empty()) v.depth = ds.depth_filenames[(size_t)i];
        camhost::Camera& c = v.cam;
        c.model = ds.camera_models[(size_t)i];
        c.tier = ds.camera_distortions[(size_t)i];
        c.width = ds.widths[(size_t)i];
        c.height = ds.heights[(size_t)i];
        c.fx = ds.intrins[(size_t)i * 4];
        c.fy = ds.intrins[(size_t)i * 4 + 1];
        c.cx = ds.intrins[(size_t)i * 4 + 2];
        c.cy = ds.intrins[(size_t)i * 4 + 3];
        std::copy_n(ds.dist_coeffs.data() + i * 8, 8, c.dist);
        if (!ds.redistort.empty()) {
            c.source_model = ds.redistort[(size_t)i].source_model;
            std::copy_n(ds.redistort[(size_t)i].params, 16, c.source_params);
        }
        // c2w is OpenGL (+Y up, -Z forward): transposing and negating rows 1
        // and 2 gives the CV world->camera rotation (as app/DenseProcessing.cpp).
        const float* pose = ds.c2w.data() + i * 12;
        double Rraw[9];
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k) Rraw[r * 3 + k] = pose[k * 4 + r] * (r == 0 ? 1.0 : -1.0);
        const double Craw[3] = {pose[3], pose[7], pose[11]};
        for (int r = 0; r < 3; ++r) {
            v.C[r] = t[r];
            for (int k = 0; k < 3; ++k) v.C[r] += A[r * 3 + k] * Craw[k];
            for (int k = 0; k < 3; ++k) {
                double acc = 0;
                for (int m = 0; m < 3; ++m) acc += Rraw[r * 3 + m] * Q[k * 3 + m];
                v.R[r * 3 + k] = acc;
            }
        }
        v.unit = 1.0 / s;
    }
    return out;
}

void to_camera(const MeshView& v, const double X[3], double out[3]) {
    const double d[3] = {X[0] - v.C[0], X[1] - v.C[1], X[2] - v.C[2]};
    for (int r = 0; r < 3; ++r)
        out[r] = v.unit * (v.R[r * 3] * d[0] + v.R[r * 3 + 1] * d[1] + v.R[r * 3 + 2] * d[2]);
}

bool project(const MeshView& v, const double X[3], double px[2], double* depth) {
    double c[3];
    to_camera(v, X, c);
    if (depth) *depth = c[2] / v.unit;
    if (v.cam.model == (int)CameraModelType::PINHOLE && c[2] <= 0) return false;
    return camhost::ray_in_frame(v.cam, c, px);
}

void camera_dir_to_file(const MeshView& v, const double c[3], double out[3]) {
    for (int r = 0; r < 3; ++r) out[r] = v.R[r] * c[0] + v.R[3 + r] * c[1] + v.R[6 + r] * c[2];
}

bool pixel_ray_file(const MeshView& v, const double px[2], double dir[3]) {
    double ray[3];
    if (!camhost::pixel_ray(v.cam, px, ray)) return false;
    camera_dir_to_file(v, ray, dir);
    const double n = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (!(n > 0)) return false;
    for (int k = 0; k < 3; ++k) dir[k] /= n;
    return true;
}

bool NormalMap::sample(const MeshView& v, const double px[2], float out[3]) const {
    if (width <= 0 || height <= 0) return false;
    const int x = (int)std::floor(px[0] * width / v.cam.width);
    const int y = (int)std::floor(px[1] * height / v.cam.height);
    if (x < 0 || y < 0 || x >= width || y >= height) return false;
    const float* p = &n[((size_t)y * width + x) * 3];
    if (p[0] == 0 && p[1] == 0 && p[2] == 0) return false;
    out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
    return true;
}

bool load_normal_map(const std::string& path, NormalMap& out, std::string& error) {
    int w = 0, h = 0, ch = 0;
    unsigned char* img = stbi_load(path.c_str(), &w, &h, &ch, 3);
    if (!img) {
        error = "cannot decode normal map " + path;
        return false;
    }
    out.width = w;
    out.height = h;
    out.n.assign((size_t)w * h * 3, 0.0f);
    for (size_t p = 0; p < (size_t)w * h; ++p) {
        float v[3];
        for (int c = 0; c < 3; ++c) v[c] = img[p * 3 + c] / 127.5f - 1.0f;
        const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (v[0] + v[1] + v[2] <= -2.366f || len < 0.25f) continue;
        for (int c = 0; c < 3; ++c) out.n[p * 3 + c] = v[c] / len;
    }
    stbi_image_free(img);
    return true;
}

bool train_to_file(const std::string& run_dir, const ViewSet& views, double M[12]) {
    spirula::SceneTransform T;
    if (!spirula::read_scene_transform_json((fs::path(run_dir) / "scene_transform.json").string(), T))
        return false;
    const spirula::SceneTransform W = T.inverse();   // train -> raw
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            double acc = 0;
            for (int k = 0; k < 3; ++k) acc += views.raw_to_file[r * 4 + k] * W.R[k * 3 + c];
            M[r * 4 + c] = acc * W.scale;
        }
        double acc = views.raw_to_file[r * 4 + 3];
        for (int k = 0; k < 3; ++k) acc += views.raw_to_file[r * 4 + k] * W.t[k];
        M[r * 4 + 3] = acc;
    }
    return true;
}

void apply_affine(const double M[12], MeshData& mesh) {
    for (auto& p : mesh.V) {
        const double x = p[0], y = p[1], z = p[2];
        for (int r = 0; r < 3; ++r)
            p[r] = (float)(M[r * 4] * x + M[r * 4 + 1] * y + M[r * 4 + 2] * z + M[r * 4 + 3]);
    }
    mesh.N.clear();
}

bool invert_affine(const double M[12], double out[12]) {
    const double A[9] = {M[0], M[1], M[2], M[4], M[5], M[6], M[8], M[9], M[10]};
    const double d = det3(A);
    if (!(std::fabs(d) > 1e-300)) return false;
    const double I[9] = {(A[4] * A[8] - A[5] * A[7]) / d, (A[2] * A[7] - A[1] * A[8]) / d, (A[1] * A[5] - A[2] * A[4]) / d,
                         (A[5] * A[6] - A[3] * A[8]) / d, (A[0] * A[8] - A[2] * A[6]) / d, (A[2] * A[3] - A[0] * A[5]) / d,
                         (A[3] * A[7] - A[4] * A[6]) / d, (A[1] * A[6] - A[0] * A[7]) / d, (A[0] * A[4] - A[1] * A[3]) / d};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) out[r * 4 + c] = I[r * 3 + c];
        out[r * 4 + 3] = -(I[r * 3] * M[3] + I[r * 3 + 1] * M[7] + I[r * 3 + 2] * M[11]);
    }
    return true;
}

bool read_matrix_file(const std::string& path, double M[12], std::string& error) {
    std::ifstream f(path);
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    std::vector<double> v;
    std::string tok;
    while (f >> tok) {
        if (tok[0] == '#') {
            std::getline(f, tok);
            continue;
        }
        char* end = nullptr;
        const double x = std::strtod(tok.c_str(), &end);
        if (end == tok.c_str() || *end) {
            error = path + ": not a number: " + tok;
            return false;
        }
        v.push_back(x);
    }
    if (v.size() != 12 && v.size() != 16) {
        error = path + ": expected a 3x4 or 4x4 matrix";
        return false;
    }
    std::copy_n(v.begin(), 12, M);
    return true;
}

}  // namespace meshing
