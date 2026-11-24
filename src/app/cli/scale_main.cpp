// `spirula scale` -- a dataset given metric scale from distances measured on
// the real object: the least-squares scale of the pairs, their spread, and
// (with --apply) every artifact rescaled. data/DatasetRescale.h.

#include "app/Tools.h"
#include "app/cli/MeshLabCli.h"

#include "app/DepthPng.h"
#include "data/DatasetRescale.h"
#include "mesh/MeshEval.h"

#include <cmath>

using spirula::i18n::format;
using namespace meshlab_cli;

namespace {

void usage() {
    intro("--dataset <dir> --pair \"x y z\" \"x y z\" <metres> [options]", M::scale_tagline,
          M::scale_usage_intro);
    help_row("--dataset <dir>", M::opt_scale_dataset);
    help_row("--pair \"x y z\" \"x y z\" <d>", M::opt_pair);
    help_row("--pairs <file>", M::opt_pairs);
    help_row("--scale <s>", M::opt_scale);
    help_row("--sigma <r>", M::opt_sigma);
    help_row("--apply", M::opt_apply);
    help_row("--splat <ply|run>", M::opt_splat);
    help_row("--mesh <file>", M::opt_scale_mesh);
    help_row("--depth-metric", M::opt_depth_metric);
    help_row("--out <dir>", M::opt_out_copy);
    outro();
}

bool save_png16(const std::string& path, const uint16_t* data, int w, int h) {
    return app::save_depth_png16(path, data, w, h);
}

}  // namespace

int spirula_scale_main(int argc, char** argv) {
    app::set_program_name(argc > 0 ? argv[0] : nullptr, "spirula scale");
    std::string dataset, pairs_file;
    std::vector<spirula::ScalePair> pairs;
    double scale = 0, sigma = -1;
    spirula::RescaleOptions opt;
    try {
        Args a(argc, argv);
        while (a.more()) {
            const std::string f = a.next();
            a.set_flag(f);
            if (f == "-h" || f == "--help") { usage(); return 0; }
            else if (f == "--dataset") dataset = a.value();
            else if (f == "--pair") {
                spirula::ScalePair p;
                a.point(p.a);
                a.point(p.b);
                p.truth = a.number();
                pairs.push_back(p);
            } else if (f == "--pairs") pairs_file = a.value();
            else if (f == "--scale") scale = a.number();
            else if (f == "--sigma") sigma = a.number();
            else if (f == "--apply") opt.apply = true;
            else if (f == "--splat") opt.splats.push_back(a.value());
            else if (f == "--mesh") opt.meshes.push_back(a.value());
            else if (f == "--depth-metric") opt.depth_metric = true;
            else if (f == "--out") opt.colmap_copy = a.value();
            else throw UsageError(format(M::err_unknown_option, {f}));
        }
        if (dataset.empty()) throw UsageError(format(M::err_missing, {"--dataset"}));
        if (scale > 0 && (!pairs.empty() || !pairs_file.empty())) throw UsageError(M::err_scale_or_pairs.get());
        if (!(scale > 0) && pairs.empty() && pairs_file.empty()) throw UsageError(format(M::err_missing, {"--pair"}));
    } catch (const UsageError& e) {
        std::fprintf(stderr, "%s\n\n", e.what());
        usage();
        return 2;
    }

    try {
        if (!pairs_file.empty()) {
            std::vector<meshing::MeasurementPair> read;
            std::string error;
            if (!meshing::read_measurements(pairs_file, read, error)) throw std::runtime_error(error);
            for (const auto& m : read) {
                spirula::ScalePair p;
                std::copy_n(m.p1, 3, p.a);
                std::copy_n(m.p2, 3, p.b);
                p.truth = m.truth;
                pairs.push_back(p);
            }
        }
        double s = scale, rel_sigma = 0;
        if (!pairs.empty()) {
            const spirula::ScaleFit fit = spirula::fit_scale(pairs);
            for (size_t i = 0; i < pairs.size(); ++i)
                std::printf("%s\n", format(M::log_pair, {(long long)(i + 1), sig(fit.measured[i]), sig(pairs[i].truth),
                                                        sig(fit.measured[i] > 0 ? pairs[i].truth / fit.measured[i] : 0),
                                                        sig(fit.residual[i])}).c_str());
            s = fit.s;
            rel_sigma = fit.sigma;
            std::printf("%s\n", format(M::log_scale, {sig(s), num(100 * fit.spread, 3), (long long)pairs.size()}).c_str());
            if (pairs.size() < 2) std::printf("%s\n", M::log_one_pair.get());
        }
        if (sigma >= 0) rel_sigma = sigma;
        opt.save_png16 = save_png16;
        spirula::RescaleReport report;
        spirula::rescale_dataset(dataset, s, rel_sigma, opt, report);
        for (const auto& p : report.rescaled)
            std::printf("%s\n", format(opt.apply ? M::log_rescaled : M::log_would_rescale, {p}).c_str());
        for (const auto& p : report.skipped) std::printf("%s\n", format(M::log_unchanged, {p}).c_str());
        if (!opt.apply) std::printf("%s\n", M::log_dry_run.get());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", format(M::err_failed, {e.what()}).c_str());
        return 1;
    }
    return 0;
}
