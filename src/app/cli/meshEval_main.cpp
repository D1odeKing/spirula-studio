// `spirula mesh-eval` -- a mesh scored against a reference mesh or point
// cloud: accuracy, completeness, F1, a per-vertex error PLY, and measured
// distances checked against true ones. docs/meshlab.md.

#include "app/Tools.h"
#include "app/cli/MeshLabCli.h"

#include "data/JsonWrite.h"
#include "data/SparseEdit.h"
#include "mesh/MeshDistance.h"
#include "mesh/MeshEval.h"
#include "mesh/MeshImport.h"
#include "mesh/MeshViews.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using spirula::i18n::format;
using namespace meshlab_cli;

namespace {

void usage() {
    intro("<mesh> --ref <file> [options]", M::eval_tagline, M::eval_usage_intro);
    help_row("--ref <file>", M::opt_ref);
    help_row("--transform <file>", M::opt_transform);
    help_row("--icp", M::opt_icp);
    help_row("--icp-iters <n>", M::opt_icp_iters);
    help_row("--run <dir> --dataset <dir>", M::opt_run);
    help_row("--crop x0 y0 z0 x1 y1 z1", M::opt_crop);
    help_row("--tau <d,d,...>", M::opt_tau);
    help_row("--max-dist <d>", M::opt_max_dist);
    help_row("--samples <n>", M::opt_samples);
    help_row("--measurements <file>", M::opt_measurements);
    help_row("--out <dir>", M::opt_out_dir);
    help_row("--error-clip <d>", M::opt_error_clip);
    help_row("--write-mesh <file>", M::opt_write_mesh);
    help_row("--write-samples <ply>", M::opt_write_samples);
    help_row("--threads <n>", M::opt_threads);
    outro();
}

}  // namespace

int spirula_mesh_eval_main(int argc, char** argv) {
    app::set_program_name(argc > 0 ? argv[0] : nullptr, "spirula mesh-eval");
    std::string mesh_path, ref_path, transform, run, dataset, measurements, out_dir, write_mesh, write_samples;
    meshing::EvalOptions opt;
    bool icp = false;
    int icp_iters = 30;
    double clip = 0;
    try {
        Args a(argc, argv);
        while (a.more()) {
            const std::string f = a.next();
            a.set_flag(f);
            if (f == "-h" || f == "--help") { usage(); return 0; }
            else if (f == "--ref") ref_path = a.value();
            else if (f == "--transform") transform = a.value();
            else if (f == "--icp") icp = true;
            else if (f == "--icp-iters") icp_iters = (int)a.integer();
            else if (f == "--run") run = a.value();
            else if (f == "--dataset") dataset = a.value();
            else if (f == "--crop") {
                a.point(opt.crop_lo);
                a.point(opt.crop_hi);
                opt.crop = true;
            } else if (f == "--tau") opt.taus = parse_list(a.value(), f);
            else if (f == "--max-dist") opt.max_dist = a.number();
            else if (f == "--samples") opt.samples = a.integer();
            else if (f == "--measurements") measurements = a.value();
            else if (f == "--out") out_dir = a.value();
            else if (f == "--error-clip") clip = a.number();
            else if (f == "--write-mesh") write_mesh = a.value();
            else if (f == "--write-samples") write_samples = a.value();
            else if (f == "--threads") opt.threads = (int)a.integer();
            else if (!f.empty() && f[0] == '-') throw UsageError(format(M::err_unknown_option, {f}));
            else if (mesh_path.empty()) mesh_path = f;
            else throw UsageError(format(M::err_unknown_option, {f}));
        }
        if (mesh_path.empty()) throw UsageError(format(M::err_missing, {"<mesh>"}));
        if (ref_path.empty() && measurements.empty() && write_mesh.empty() && write_samples.empty()) throw UsageError(format(M::err_missing, {"--ref"}));
        if (!run.empty() && dataset.empty()) throw UsageError(format(M::err_missing, {"--dataset"}));
    } catch (const UsageError& e) {
        std::fprintf(stderr, "%s\n\n", e.what());
        usage();
        return 2;
    }

    try {
        meshing::MeshData mesh;
        std::string error;
        if (!meshing::read_mesh(mesh_path, mesh, error)) throw std::runtime_error(error);
        std::printf("%s\n", format(M::log_mesh, {mesh_path, (long long)mesh.V.size(), (long long)mesh.F.size()}).c_str());
        if (!run.empty()) {
            const meshing::ViewSet views = meshing::load_views(dataset);
            double M34[12];
            if (!meshing::train_to_file(run, views, M34))
                throw std::runtime_error(format(M::err_no_scene_transform, {run}));
            meshing::apply_affine(M34, mesh);
        }
        if (!transform.empty()) {
            double M34[12];
            if (!meshing::read_matrix_file(transform, M34, error)) throw std::runtime_error(error);
            meshing::apply_affine(M34, mesh);
        }

        if (!write_mesh.empty()) {
            std::string ext = fs::path(write_mesh).extension().string();
            if (!ext.empty()) ext.erase(0, 1);
            const meshing::MeshFormatSpec spec = meshing::parse_one_mesh_format(ext.empty() ? "ply" : ext);
            meshing::MeshColorMode mode = !mesh.UV.empty() && !mesh.texture.empty() ? meshing::MeshColorMode::Texture
                                        : !mesh.C.empty() ? meshing::MeshColorMode::Vertex : meshing::MeshColorMode::None;
            if (!meshing::check_export_support(spec, mode).empty()) mode = meshing::MeshColorMode::None;
            meshing::compute_vertex_normals(mesh);
            meshing::write_mesh(mesh, mode, spec, meshing::mesh_output_strip_ext(write_mesh), false);
            std::printf("%s\n", format(M::log_wrote, {write_mesh}).c_str());
        }
        if (!write_samples.empty()) {
            std::vector<double> xyz;
            meshing::sample_surface(mesh, opt.samples, opt.seed, xyz);
            spirula::write_ply_points(write_samples, xyz.data(), nullptr, (int64_t)xyz.size() / 3, nullptr, nullptr,
                                      spirula::PlyCoordinates::Float64);
            std::printf("%s\n", format(M::log_wrote, {write_samples}).c_str());
        }
        if (ref_path.empty() && measurements.empty()) return 0;

        if (out_dir.empty()) out_dir = (fs::path(mesh_path).parent_path() / (fs::path(mesh_path).stem().string() + "_eval")).string();
        fs::create_directories(out_dir);
        JsonWriter j;
        j.object();
        j.field("mesh", fs::absolute(mesh_path).string());

        if (!ref_path.empty()) {
            meshing::EvalReference ref;
            if (!meshing::load_reference(ref_path, ref, error)) throw std::runtime_error(error);
            std::printf("%s\n", format(M::log_reference, {ref_path, (long long)(ref.is_mesh() ? ref.mesh.V.size() : ref.points.size() / 3),
                                                           (long long)ref.mesh.F.size()}).c_str());
            double M34[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
            if (icp) {
                const double rms = meshing::icp_point_to_plane(mesh, ref, M34, icp_iters, opt.threads);
                meshing::apply_affine(M34, mesh);
                std::printf("%s\n", format(M::log_icp, {sig(rms)}).c_str());
            }
            const meshing::EvalResult r = meshing::evaluate_mesh(mesh, ref, opt);
            std::printf("%s\n", format(M::res_accuracy, {sig(r.acc_mean), sig(r.acc_median)}).c_str());
            std::printf("%s\n", format(M::res_completeness, {sig(r.comp_mean), sig(r.comp_median)}).c_str());
            std::printf("%s\n", format(M::res_chamfer, {sig(r.chamfer)}).c_str());
            for (const auto& row : r.f1)
                std::printf("%s\n", format(M::res_f1, {sig(row.tau), num(100 * row.precision, 2), num(100 * row.recall, 2),
                                                       num(100 * row.f1, 2)}).c_str());
            j.field("reference", fs::absolute(ref_path).string());
            j.field("accuracy_mean", r.acc_mean).field("accuracy_median", r.acc_median);
            j.field("completeness_mean", r.comp_mean).field("completeness_median", r.comp_median);
            j.field("chamfer", r.chamfer);
            j.field("mesh_samples", (long long)r.mesh_points).field("reference_points", (long long)r.ref_points);
            j.key("f1").array();
            for (const auto& row : r.f1)
                j.object().field("tau", row.tau).field("precision", row.precision).field("recall", row.recall)
                    .field("f1", row.f1).end();
            j.end();
            j.key("alignment").array();
            for (double v : M34) j.value(v);
            j.end();

            if (!(clip > 0)) {
                clip = opt.taus.empty() ? 0 : *std::max_element(opt.taus.begin(), opt.taus.end());
                if (!(clip > 0)) {
                    std::vector<float> e = r.vertex_error;
                    if (!e.empty()) {
                        std::nth_element(e.begin(), e.begin() + (ptrdiff_t)(0.95 * (e.size() - 1)), e.end());
                        clip = e[(size_t)(0.95 * (e.size() - 1))];
                    }
                }
            }
            meshing::MeshData colored = mesh;
            colored.C.resize(colored.V.size());
            for (size_t i = 0; i < colored.V.size(); ++i)
                colored.C[i] = meshing::error_color(r.vertex_error[i], (float)clip);
            colored.UV.clear(); colored.texture.clear(); colored.N.clear();
            const std::string base = (fs::path(out_dir) / (fs::path(mesh_path).stem().string() + "_error")).string();
            meshing::write_mesh(colored, meshing::MeshColorMode::Vertex, meshing::parse_one_mesh_format("ply"), base, false);
            j.field("error_ply", base + ".ply").field("error_clip", clip);
            std::printf("%s\n", format(M::log_wrote, {base + ".ply"}).c_str());
        }

        if (!measurements.empty()) {
            std::vector<meshing::MeasurementPair> pairs;
            if (!meshing::read_measurements(measurements, pairs, error)) throw std::runtime_error(error);
            meshing::measure_pairs(mesh, pairs);
            double sum = 0, worst = 0;
            j.key("measurements").array();
            for (const auto& p : pairs) {
                const double e = p.snapped - p.truth;
                sum += std::fabs(e);
                worst = std::max(worst, std::fabs(e));
                std::printf("%s\n", format(M::res_pair, {p.name, sig(p.truth), sig(p.measured), sig(p.snapped), sig(e)}).c_str());
                j.object().field("name", p.name).field("true_distance", p.truth).field("measured", p.measured)
                    .field("on_mesh", p.snapped).field("error", e).end();
            }
            j.end();
            const double mean = pairs.empty() ? 0 : sum / pairs.size();
            std::printf("%s\n", format(M::res_pairs_summary, {sig(mean), sig(worst)}).c_str());
            j.field("measurement_error_mean", mean).field("measurement_error_max", worst);
        }
        j.end();
        const fs::path metrics = fs::path(out_dir) / "metrics.json";
        std::ofstream f(metrics, std::ios::trunc);
        f << j.str();
        if (!f) throw std::runtime_error("cannot write " + metrics.string());
        std::printf("%s\n", format(M::log_wrote, {metrics.string()}).c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", format(M::err_failed, {e.what()}).c_str());
        return 1;
    }
    return 0;
}
