// `spirula mesh-refine` -- any mesh (splat or dense) reshaped by the dataset's
// normal maps, anchored to measured points (mesh/NormalRefine.h). Writes the
// refined mesh, a displacement PLY and a report of where the prior moved it.

#include "app/Tools.h"
#include "app/cli/MeshLabCli.h"

#include "data/DatasetParser.h"
#include "data/JsonWrite.h"
#include "dense/Generation.h"
#include "mesh/MeshEval.h"
#include "mesh/MeshImport.h"
#include "mesh/MeshLog.h"
#include "mesh/MeshViews.h"
#include "mesh/NormalRefine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using spirula::i18n::format;
using namespace meshlab_cli;

namespace {

void usage() {
    intro("--mesh <file> --dataset <dir> [options]", M::refine_tagline, M::refine_usage_intro);
    help_row("--mesh <file>", M::opt_refine_mesh);
    help_row("--dataset <dir>", M::opt_refine_dataset);
    help_row("--run <dir>", M::opt_refine_run);
    help_row("--keep-frame", M::opt_keep_frame);
    help_row("--anchor dense|none", M::opt_anchor);
    help_row("--cloud <ply>", M::opt_anchor_cloud);
    help_row("--normals <dir>", M::opt_normals);
    help_row("--depths <dir>", M::opt_depths);
    help_row("-o, --output <file>", M::opt_refine_output);
    help_row("--max-shift <d>", M::opt_max_shift);
    help_row("--lambda <w>", M::opt_lambda);
    help_row("--pull <t>", M::opt_pull);
    help_row("--normal-iters <n>", M::opt_normal_iters);
    help_row("--vertex-iters <n>", M::opt_vertex_iters);
    help_row("--rounds <n>", M::opt_rounds);
    help_row("--outlier-deg <a>", M::opt_outlier_deg);
    help_row("--raster <px>", M::opt_raster);
    help_row("--plane-snap", M::opt_plane_snap);
    help_row("--plane-tol <d>", M::opt_plane_tol);
    help_row("--plane-deg <a>", M::opt_plane_deg);
    help_row("--threads <n>", M::opt_threads);
    outro();
}

// x y z, an error colour, and the signed shift as a float property: the
// displacement map as something a viewer shows and a script reads.
void write_shift_ply(const meshing::MeshData& m, const std::vector<float>& shift, double clip, const std::string& path) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << "ply\nformat binary_little_endian 1.0\nelement vertex " << m.V.size()
      << "\nproperty float x\nproperty float y\nproperty float z\n"
         "property uchar red\nproperty uchar green\nproperty uchar blue\nproperty float shift\n"
         "element face " << m.F.size() << "\nproperty list uchar int vertex_indices\nend_header\n";
    for (size_t i = 0; i < m.V.size(); ++i) {
        f.write(reinterpret_cast<const char*>(m.V[i].data()), 12);
        const auto c = meshing::error_color(std::fabs(shift[i]), (float)clip);
        f.write(reinterpret_cast<const char*>(c.data()), 3);
        f.write(reinterpret_cast<const char*>(&shift[i]), 4);
    }
    for (const auto& t : m.F) {
        const unsigned char three = 3;
        f.write(reinterpret_cast<const char*>(&three), 1);
        f.write(reinterpret_cast<const char*>(t.data()), 12);
    }
    if (!f) throw std::runtime_error("cannot write " + path);
}

}  // namespace

int spirula_mesh_refine_main(int argc, char** argv) {
    app::set_program_name(argc > 0 ? argv[0] : nullptr, "spirula mesh-refine");
    std::string mesh_path, dataset, run, anchor = "dense", cloud, output;
    bool keep_frame = false;
    meshing::RefineConfig cfg;
    meshing::ViewOptions view_opt;
    try {
        Args a(argc, argv);
        while (a.more()) {
            const std::string f = a.next();
            a.set_flag(f);
            if (f == "-h" || f == "--help") { usage(); return 0; }
            else if (f == "--mesh") mesh_path = a.value();
            else if (view_dir_flag(f, a, view_opt)) {}
            else if (f == "--dataset") dataset = a.value();
            else if (f == "--run") run = a.value();
            else if (f == "--keep-frame") keep_frame = true;
            else if (f == "--anchor") {
                anchor = a.value();
                if (anchor != "dense" && anchor != "none") throw UsageError(format(M::err_bad_value, {anchor, f}));
            } else if (f == "--cloud") cloud = a.value();
            else if (f == "-o" || f == "--output") output = a.value();
            else if (f == "--max-shift") cfg.max_shift = a.number();
            else if (f == "--lambda") cfg.lambda = (float)a.number();
            else if (f == "--pull") cfg.pull = (float)a.number();
            else if (f == "--normal-iters") cfg.normal_iters = (int)a.integer();
            else if (f == "--vertex-iters") cfg.vertex_iters = (int)a.integer();
            else if (f == "--rounds") cfg.rounds = (int)a.integer();
            else if (f == "--outlier-deg") cfg.outlier_deg = (float)a.number();
            else if (f == "--raster") cfg.raster = (int)a.integer();
            else if (f == "--plane-snap") cfg.plane_snap = true;
            else if (f == "--plane-tol") cfg.plane_tol = a.number();
            else if (f == "--plane-deg") cfg.plane_deg = (float)a.number();
            else if (f == "--threads") cfg.threads = (int)a.integer();
            else throw UsageError(format(M::err_unknown_option, {f}));
        }
        if (mesh_path.empty()) throw UsageError(format(M::err_missing, {"--mesh"}));
        if (dataset.empty()) throw UsageError(format(M::err_missing, {"--dataset"}));
        if (cfg.max_shift < 0 || cfg.lambda <= 0) throw UsageError(format(M::err_bad_value, {"", "--max-shift/--lambda"}));
    } catch (const UsageError& e) {
        std::fprintf(stderr, "%s\n\n", e.what());
        usage();
        return 2;
    }

    try {
        const auto t0 = std::chrono::steady_clock::now();
        meshing::MeshData mesh;
        std::string error;
        if (!meshing::read_mesh(mesh_path, mesh, error)) throw std::runtime_error(error);
        std::printf("%s\n", format(M::log_mesh, {mesh_path, (long long)mesh.V.size(), (long long)mesh.F.size()}).c_str());
        const meshing::ViewSet views = meshing::load_views(dataset, view_opt);
        print_maps(views);
        double back[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        std::string frame = "file";
        if (!run.empty()) {
            double M34[12];
            if (!meshing::train_to_file(run, views, M34)) throw std::runtime_error(format(M::err_no_scene_transform, {run}));
            meshing::apply_affine(M34, mesh);
            meshing::invert_affine(M34, back);
        }
        int with_normals = 0;
        for (const auto& v : views.views) with_normals += !v.normal.empty();
        if (!with_normals) throw std::runtime_error(format(M::err_no_normals, {(fs::path(dataset) / view_opt.normal_dir).string()}));

        std::vector<double> anchors;
        if (anchor == "dense") {
            if (cloud.empty()) cloud = spirula::dense::artifact_files(dataset).cloud.string();
            if (fs::is_regular_file(cloud)) {
                anchors = read_ply_points(cloud).xyz;
                std::printf("%s\n", format(M::log_cloud, {cloud, (long long)(anchors.size() / 3)}).c_str());
            } else {
                std::fprintf(stderr, "%s\n", format(M::warn_no_cloud, {cloud}).c_str());
            }
        }

        if (output.empty()) {
            const fs::path p(mesh_path);
            output = (p.parent_path() / (p.stem().string() + "_refined" + p.extension().string())).string();
        }
        const std::string base = meshing::mesh_output_strip_ext(output);
        std::string ext = fs::path(output).extension().string();
        if (!ext.empty()) ext.erase(0, 1);
        if (ext.empty()) ext = "ply";
        const meshing::MeshFormatSpec spec = meshing::parse_one_mesh_format(ext);
        meshing::MeshColorMode mode = !mesh.UV.empty() && !mesh.texture.empty() ? meshing::MeshColorMode::Texture
                                    : !mesh.C.empty() ? meshing::MeshColorMode::Vertex : meshing::MeshColorMode::None;
        if (!meshing::check_export_support(spec, mode).empty()) mode = meshing::MeshColorMode::None;

        meshing::RefineStats st;
        std::vector<float> shift;
        meshing::refine_with_normals(mesh, views, anchors, cfg, st, shift, [](const char* s) {
            using meshing::mlog::Stage;
            const std::string k = s;
            if (k == "guides") meshing::mlog::out(Stage::Occupancy, M::stage_guides);
            else if (k == "filter") meshing::mlog::out(Stage::Merge, M::stage_filter);
            else if (k == "vertices") meshing::mlog::out(Stage::Quality, M::stage_vertices);
            else meshing::mlog::out(Stage::Orient, M::stage_planes);
            std::fflush(stdout);
        });
        std::printf("%s\n", format(M::log_guides, {(long long)st.views, (long long)st.guided, (long long)mesh.F.size(),
                                                   num(st.mean_conf, 3)}).c_str());
        std::printf("%s\n", format(M::log_shift, {sig(st.mean_shift), sig(st.max_shift), sig(st.cap)}).c_str());
        if (cfg.plane_snap) std::printf("%s\n", format(M::log_planes, {(long long)st.planes, (long long)st.snapped}).c_str());

        if (keep_frame && !run.empty()) {
            meshing::apply_affine(back, mesh);
            frame = "train";
        }
        meshing::compute_vertex_normals(mesh);
        meshing::write_mesh(mesh, mode, spec, base, false);
        meshing::mlog::out(meshing::mlog::Stage::Wrote, M::log_wrote, {meshing::mesh_output_paths(spec, mode, base).front()});
        const std::string shift_path = base + "_shift.ply";
        write_shift_ply(mesh, shift, st.cap, shift_path);
        std::printf("%s\n", format(M::log_wrote, {shift_path}).c_str());

        JsonWriter j;
        j.object();
        j.field("mesh", fs::absolute(mesh_path).string()).field("output", meshing::mesh_output_paths(spec, mode, base).front());
        j.field("frame", frame).field("anchor", anchor).field("views_with_normals", st.views);
        j.field("guided_faces", (long long)st.guided).field("faces", (long long)mesh.F.size());
        j.field("mean_confidence", st.mean_conf).field("guide_angle_mean_deg", st.guide_angle_deg).field("mean_shift", st.mean_shift).field("max_shift", st.max_shift);
        j.field("shift_cap", st.cap).field("planes", st.planes).field("snapped_vertices", (long long)st.snapped);
        j.end();
        const std::string report = base + "_refine.json";
        std::ofstream f(report, std::ios::trunc);
        f << j.str();
        if (!f) throw std::runtime_error("cannot write " + report);
        std::printf("%s\n", format(M::log_wrote, {report}).c_str());
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        meshing::mlog::out(meshing::mlog::Stage::Done, spirula::i18n::msg::mesh::done_summary,
                           {(long long)mesh.V.size(), (long long)mesh.F.size(), num(secs, 1)});
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", format(M::err_failed, {e.what()}).c_str());
        return 1;
    }
    return 0;
}
