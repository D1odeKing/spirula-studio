#include "data/DatasetRescale.h"

#include "checkpoint/SplatPly.h"
#include "checkpoint/SplatTransform.h"
#include "core/Similarity.h"
#include "data/DatasetParser.h"
#include "data/SceneTransform.h"
#include "data/SparseEdit.h"
#include "dense/Generation.h"
#include "external/stb_image.h"
#include "mesh/MeshExport.h"
#include "mesh/MeshImport.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace spirula {

namespace fs = std::filesystem;

namespace {

void keep_original(const fs::path& p) {
    std::error_code ec;
    const fs::path bak = p.string() + ".orig";
    if (fs::exists(bak, ec) || !fs::exists(p, ec)) return;
    fs::copy_file(p, bak, ec);
}

double dist(const double a[3], const double b[3]) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

bool invert3(const double A[9], double out[9]) {
    const double d = A[0] * (A[4] * A[8] - A[5] * A[7]) - A[1] * (A[3] * A[8] - A[5] * A[6]) +
                     A[2] * (A[3] * A[7] - A[4] * A[6]);
    if (!(std::fabs(d) > 1e-300)) return false;
    out[0] = (A[4] * A[8] - A[5] * A[7]) / d; out[1] = (A[2] * A[7] - A[1] * A[8]) / d; out[2] = (A[1] * A[5] - A[2] * A[4]) / d;
    out[3] = (A[5] * A[6] - A[3] * A[8]) / d; out[4] = (A[0] * A[8] - A[2] * A[6]) / d; out[5] = (A[2] * A[3] - A[0] * A[5]) / d;
    out[6] = (A[3] * A[7] - A[4] * A[6]) / d; out[7] = (A[1] * A[6] - A[0] * A[7]) / d; out[8] = (A[0] * A[4] - A[1] * A[3]) / d;
    return true;
}

// gauge.txt with `metric 1` and the measured scale, every other line kept.
std::string rewrite_gauge(const fs::path& path, double sigma) {
    std::vector<std::string> kept;
    std::ifstream in(path);
    std::string line;
    bool header = false;
    while (in && std::getline(in, line)) {
        std::istringstream ss(line);
        std::string key;
        ss >> key;
        if (key == "metric" || key == "scale" || key == "scale_sigma") continue;
        if (!key.empty() && key[0] == '#') header = true;
        kept.push_back(line);
    }
    std::ostringstream out;
    if (!header) out << "# What this dataset's frame means.\n";
    for (const auto& l : kept) out << l << "\n";
    out << "metric 1\nscale measured\n";
    if (sigma > 0) out << "scale_sigma " << sigma << "\n";
    return out.str();
}

meshing::MeshColorMode mesh_mode(const meshing::MeshData& m) {
    if (!m.UV.empty() && !m.texture.empty()) return meshing::MeshColorMode::Texture;
    if (!m.C.empty()) return meshing::MeshColorMode::Vertex;
    return meshing::MeshColorMode::None;
}

}  // namespace

ScaleFit fit_scale(const std::vector<ScalePair>& pairs) {
    ScaleFit f;
    double dm = 0, mm = 0;
    for (const ScalePair& p : pairs) {
        const double m = dist(p.a, p.b);
        f.measured.push_back(m);
        if (m > 0 && p.truth > 0) { dm += p.truth * m; mm += m * m; }
    }
    if (!(mm > 0)) throw std::runtime_error("no pair has a positive measured and true length");
    f.s = dm / mm;
    std::vector<double> scales;
    for (size_t i = 0; i < pairs.size(); ++i) {
        f.residual.push_back(f.s * f.measured[i] - pairs[i].truth);
        if (f.measured[i] > 0 && pairs[i].truth > 0) scales.push_back(pairs[i].truth / f.measured[i]);
    }
    if (scales.size() >= 2) {
        double mean = 0, var = 0;
        for (double v : scales) mean += v / scales.size();
        for (double v : scales) var += (v - mean) * (v - mean) / (scales.size() - 1);
        f.spread = std::sqrt(var) / f.s;
        f.sigma = f.spread / std::sqrt((double)scales.size());
    }
    return f;
}

void rescale_dataset(const std::string& dir, double s, double sigma,
                     const RescaleOptions& opt, RescaleReport& report) {
    if (!(s > 0) || !std::isfinite(s)) throw std::runtime_error("scale must be a positive number");
    DatasetParserConfig cfg;
    cfg.require_image_files = false;
    cfg.eval_mode = "all";
    cfg.center_mode = "none";
    const ParsedDataset ds = parse_dataset(dir, cfg, "");

    // s about the file frame's origin, written in the RAW frame the sparse
    // writers take: raw' = s raw + (s - 1) A^-1 t for file = A raw + t.
    double A[9], Ai[9], t[3];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) A[r * 3 + c] = ds.raw_to_file[r * 4 + c];
        t[r] = ds.raw_to_file[r * 4 + 3];
    }
    if (!invert3(A, Ai)) throw std::runtime_error("dataset raw-to-file transform is singular");
    Sim3 raw;
    raw.s = s;
    for (int r = 0; r < 3; ++r)
        raw.t[r] = (s - 1) * (Ai[r * 3] * t[0] + Ai[r * 3 + 1] * t[1] + Ai[r * 3 + 2] * t[2]);
    Sim3 file;
    file.s = s;

    // ---- cameras + sparse points ----
    const SparseFormat fmt = sparse_format_of(dir);
    if (fmt == SparseFormat::None) {
        report.skipped.push_back(dir + ": no sparse reconstruction");
    } else {
        SparseKeep keep;
        keep.points.assign((size_t)std::max<int64_t>(0, ds.points.num()), 1);
        if (!opt.colmap_copy.empty()) {
            if (fmt != SparseFormat::Colmap) throw std::runtime_error("a rescaled copy needs a COLMAP dataset");
            if (opt.apply) {
                for (auto& p : sparse_write_copy(dir, opt.colmap_copy, keep, &raw)) report.rescaled.push_back(p);
            } else {
                report.rescaled.push_back((fs::path(opt.colmap_copy) / "sparse" / "0").string());
            }
        } else if (opt.apply) {
            for (auto& p : sparse_write_filtered(dir, keep, &raw)) report.rescaled.push_back(p);
        } else {
            report.rescaled.push_back(fmt == SparseFormat::Colmap ? find_colmap_model(dir, "") : dir + "/transforms.json");
        }
    }

    // ---- gauge.txt ----
    {
        const std::string model = fmt == SparseFormat::Colmap ? find_colmap_model(dir, "") : std::string();
        fs::path base = model.empty() ? fs::path(dir) : fs::path(model);
        if (!opt.colmap_copy.empty()) base = fs::path(opt.colmap_copy) / "sparse" / "0";
        const fs::path gauge = base / "gauge.txt";
        if (opt.apply) {
            const std::string text = rewrite_gauge(fs::path(model.empty() ? dir : model) / "gauge.txt", sigma);
            keep_original(gauge);
            std::ofstream f(gauge, std::ios::trunc);
            f << text;
            if (!f) throw std::runtime_error("cannot write " + gauge.string());
        }
        report.rescaled.push_back(gauge.string());
    }

    // ---- dense cloud ----
    if (opt.colmap_copy.empty()) {
        dense::ArtifactFiles files;
        bool have = false;
        try {
            files = dense::artifact_files(dir);
            have = fs::is_regular_file(files.cloud);
        } catch (const std::exception& e) {
            report.skipped.push_back(std::string("dense: ") + e.what());
        }
        if (have) {
            if (opt.apply) {
                const ColmapPoints3D cloud = read_ply_points(files.cloud.string());
                const fs::path tmp = files.cloud.parent_path() / "roma-rescale.part";
                write_ply_points(tmp.string(), cloud.xyz.data(), cloud.rgb.empty() ? nullptr : cloud.rgb.data(),
                                 cloud.num(), nullptr, &file, PlyCoordinates::Float64);
                dense::commit_edit(dir, files, tmp, cloud.num());
            }
            report.rescaled.push_back(files.cloud.string());
        }
    } else {
        report.skipped.push_back("dense: not copied with --out");
    }

    // ---- splat models: each in its run's training frame ----
    for (const std::string& path : opt.splats) {
        auto [ply, run_dir] = find_splat_ply(path);
        // S_train = T S_raw T^-1 for p_train = k R p_raw + c: scale s, shift (1 - s) c + k R u.
        SceneTransform T;
        const bool framed = !run_dir.empty() &&
            read_scene_transform_json((fs::path(run_dir) / "scene_transform.json").string(), T);
        Sim3 S;
        S.s = s;
        for (int r = 0; r < 3; ++r) {
            const double Ru = framed ? T.R[r * 3] * raw.t[0] + T.R[r * 3 + 1] * raw.t[1] + T.R[r * 3 + 2] * raw.t[2]
                                     : raw.t[r];
            S.t[r] = framed ? (1 - s) * T.t[r] + T.scale * Ru : Ru;
        }
        if (opt.apply) {
            SplatCloud c = read_splat_ply(ply);
            transform_splats(c, S);
            keep_original(ply);
            write_splat_ply(c, ply);
        }
        report.rescaled.push_back(ply);
    }

    // ---- meshes, file frame ----
    for (const std::string& path : opt.meshes) {
        if (opt.apply) {
            meshing::MeshData m;
            std::string error;
            if (!meshing::read_mesh(path, m, error)) throw std::runtime_error(error);
            for (auto& v : m.V)
                for (float& x : v) x = (float)(x * s);
            std::string ext = fs::path(path).extension().string();
            if (!ext.empty()) ext.erase(0, 1);
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            const meshing::MeshFormatSpec spec = meshing::parse_one_mesh_format(ext);
            keep_original(path);
            meshing::write_mesh(m, mesh_mode(m), spec, meshing::mesh_output_strip_ext(path), false);
        }
        report.rescaled.push_back(path);
    }

    // ---- depth and normal maps ----
    size_t depths = 0;
    for (const auto& d : ds.depth_filenames) depths += !d.empty();
    if (depths && opt.depth_metric && opt.save_png16) {
        for (const auto& d : ds.depth_filenames) {
            if (d.empty()) continue;
            if (opt.apply) {
                int w = 0, h = 0, ch = 0;
                stbi_us* img = stbi_load_16(d.c_str(), &w, &h, &ch, 1);
                if (!img) throw std::runtime_error("cannot decode depth map " + d);
                std::vector<uint16_t> out((size_t)w * h);
                for (size_t i = 0; i < out.size(); ++i)
                    out[i] = (uint16_t)std::min(65535.0, std::round(img[i] * s));
                stbi_image_free(img);
                keep_original(d);
                if (!opt.save_png16(d, out.data(), w, h)) throw std::runtime_error("cannot write " + d);
            }
            report.rescaled.push_back(d);
        }
    } else if (depths) {
        report.skipped.push_back("depths: " + std::to_string(depths) + " maps, treated as normalized");
    }
    size_t normals = 0;
    for (const auto& n : ds.normal_filenames) normals += !n.empty();
    if (normals) report.skipped.push_back("normals: " + std::to_string(normals) + " maps, unchanged by a scale");
}

}  // namespace spirula
