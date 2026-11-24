// `spirula mesh-dense` -- a mesh from the dataset's dense RoMa cloud by a
// visibility graph cut (mesh/GraphCutMesher.h), in the file frame.
// A separate subcommand so `spirula mesh` stays the splat mesher it is.

#include "app/Tools.h"
#include "app/cli/MeshLabCli.h"

#include "data/DatasetParser.h"
#include "dense/Generation.h"
#include "mesh/GraphCutMesher.h"
#include "mesh/MeshLog.h"
#include "mesh/MeshViews.h"

#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;
using spirula::i18n::format;
using namespace meshlab_cli;

namespace {

void usage() {
    intro("--dataset <dir> [options]", M::dense_tagline, M::dense_usage_intro);
    help_row("--dataset <dir>", M::opt_dense_dataset);
    help_row("--cloud <ply>", M::opt_cloud);
    help_row("--normals <dir>", M::opt_normals);
    help_row("--depths <dir>", M::opt_depths);
    help_row("-o, --output <base>", M::opt_output);
    help_row("--format <list>", M::opt_format);
    help_row("--color vertex|none", M::opt_color);
    help_row("--smooth <n>", M::opt_smooth);
    help_row("--smooth-k <k>", M::opt_smooth_k);
    help_row("--voxel <d>", M::opt_voxel);
    help_row("--max-points <n>", M::opt_max_points);
    help_row("--rays <n>", M::opt_rays);
    help_row("--grid <n>", M::opt_grid);
    help_row("--depth-eps <r>", M::opt_depth_eps);
    help_row("--sigma <k>", M::opt_sigma_dense);
    help_row("--quality <w>", M::opt_quality);
    help_row("--hull <w>", M::opt_hull);
    help_row("--long-edge <k>", M::opt_long_edge);
    help_row("--floaters <n>", M::opt_floaters);
    help_row("--threads <n>", M::opt_threads);
    outro();
}

// Stages the GUI's MeshRunner already places on its bar, in increasing order.
void stage_line(const std::string& s) {
    using meshing::mlog::Stage;
    const Stage st = s == "smooth" || s == "thin" ? Stage::Loading : s == "visibility" ? Stage::PointCloud
                   : s == "delaunay" ? Stage::Delaunay : s == "rays" ? Stage::Occupancy
                   : s == "mincut" ? Stage::CutEdges : s == "extract" ? Stage::Marching : Stage::Cleanup;
    const spirula::i18n::Msg& m = s == "smooth" ? M::stage_smooth : s == "thin" ? M::stage_thin : s == "visibility" ? M::stage_visibility
                                : s == "delaunay" ? M::stage_delaunay : s == "rays" ? M::stage_rays
                                : s == "mincut" ? M::stage_mincut : s == "extract" ? M::stage_extract : M::stage_cleanup;
    meshing::mlog::out(st, m);
    std::fflush(stdout);
}

}  // namespace

int spirula_mesh_dense_main(int argc, char** argv) {
    app::set_program_name(argc > 0 ? argv[0] : nullptr, "spirula mesh-dense");
    std::string dataset, cloud, output, formats = "ply", color = "vertex";
    meshing::GraphCutConfig cfg;
    meshing::ViewOptions view_opt;
    cfg.verbose = false;
    try {
        Args a(argc, argv);
        while (a.more()) {
            const std::string f = a.next();
            a.set_flag(f);
            if (f == "-h" || f == "--help") { usage(); return 0; }
            else if (view_dir_flag(f, a, view_opt)) {}
            else if (f == "--dataset") dataset = a.value();
            else if (f == "--cloud") cloud = a.value();
            else if (f == "-o" || f == "--output") output = a.value();
            else if (f == "--format") formats = a.value();
            else if (f == "--color") {
                color = a.value();
                if (color != "vertex" && color != "none") throw UsageError(format(M::err_bad_value, {color, f}));
            } else if (f == "--smooth") cfg.smooth_iters = (int)a.integer();
            else if (f == "--smooth-k") cfg.smooth_k = (int)a.integer();
            else if (f == "--voxel") cfg.voxel = a.number();
            else if (f == "--max-points") cfg.max_points = a.integer();
            else if (f == "--rays") cfg.max_rays = (int)a.integer();
            else if (f == "--grid") cfg.grid = (int)a.integer();
            else if (f == "--depth-eps") cfg.depth_eps = a.number();
            else if (f == "--sigma") cfg.sigma = (float)a.number();
            else if (f == "--quality") cfg.quality = (float)a.number();
            else if (f == "--hull") cfg.hull = (float)a.number();
            else if (f == "--long-edge") cfg.long_edge = (float)a.number();
            else if (f == "--floaters") cfg.floater_min_faces = (int)a.integer();
            else if (f == "--threads") cfg.threads = (int)a.integer();
            else if (f == "--verbose") cfg.verbose = true;
            else throw UsageError(format(M::err_unknown_option, {f}));
        }
        if (dataset.empty()) throw UsageError(format(M::err_missing, {"--dataset"}));
        if (cfg.max_rays < 1 || cfg.grid < 8 || cfg.sigma <= 0) throw UsageError(format(M::err_bad_value, {"", "--rays/--grid/--sigma"}));
    } catch (const UsageError& e) {
        std::fprintf(stderr, "%s\n\n", e.what());
        usage();
        return 2;
    }

    try {
        const auto t0 = std::chrono::steady_clock::now();
        if (cloud.empty()) cloud = spirula::dense::artifact_files(dataset).cloud.string();
        if (output.empty()) output = (fs::path(dataset) / "mesh" / "dense_mesh").string();
        const auto specs = meshing::parse_mesh_formats(formats);
        const meshing::MeshColorMode mode = color == "none" ? meshing::MeshColorMode::None : meshing::MeshColorMode::Vertex;
        // A format that cannot carry the color (STL, OBJ) is written shape-only.
        auto mode_for = [mode](const meshing::MeshFormatSpec& spec) {
            return meshing::check_export_support(spec, mode).empty() ? mode : meshing::MeshColorMode::None;
        };
        for (const auto& spec : specs) {
            const std::string why = meshing::check_export_support(spec, mode_for(spec));
            if (!why.empty()) throw std::runtime_error(why);
        }
        ColmapPoints3D pts = read_ply_points(cloud);
        std::printf("%s\n", format(M::log_cloud, {cloud, (long long)pts.num()}).c_str());
        if (cfg.smooth_iters > 0 && cfg.smooth_k > 0) {
            stage_line("smooth");
            meshing::smooth_points(pts.xyz, cfg);
        }
        const meshing::ViewSet views = meshing::load_views(dataset, view_opt);
        print_maps(views);
        std::printf("%s\n", format(M::log_cameras, {(long long)views.views.size()}).c_str());

        meshing::GraphCutStats st;
        meshing::MeshData mesh = meshing::graph_cut_mesh(pts.xyz, pts.rgb, views, cfg, st,
                                                         [](const char* s) { stage_line(s); });
        std::printf("%s\n", format(M::log_dense_result, {(long long)st.points, (long long)st.rays, (long long)st.tets,
                                                         (long long)st.inside}).c_str());
        std::printf("%s\n", format(M::log_dense_faces, {(long long)st.faces, (long long)st.long_faces}).c_str());
        if (mesh.F.empty()) throw std::runtime_error(M::err_empty_mesh.get());
        if (mode == meshing::MeshColorMode::Vertex) {
            meshing::mlog::out(meshing::mlog::Stage::Color, M::stage_color);
            meshing::color_from_points(mesh, pts.xyz, pts.rgb, cfg.threads);
        }
        meshing::compute_vertex_normals(mesh);
        fs::create_directories(fs::path(output).parent_path().empty() ? fs::path(".") : fs::path(output).parent_path());
        const std::string base = meshing::mesh_output_strip_ext(output);
        for (const auto& spec : specs) {
            meshing::write_mesh(mesh, mode_for(spec), spec, base, false);
            meshing::mlog::out(meshing::mlog::Stage::Wrote, M::log_wrote,
                               {meshing::mesh_output_paths(spec, mode_for(spec), base).front()});
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        meshing::mlog::out(meshing::mlog::Stage::Done, spirula::i18n::msg::mesh::done_summary,
                           {(long long)mesh.V.size(), (long long)mesh.F.size(), num(secs, 1)});
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", format(M::err_failed, {e.what()}).c_str());
        return 1;
    }
    return 0;
}
