#pragma once

// Metric scale for a dataset from distances someone measured: a scale factor
// fitted to point pairs, and the similarity p -> s p (FILE frame, about its
// origin) applied to everything a dataset holds. docs/meshlab.md, "Scale".

#include <cstdint>
#include <string>
#include <vector>

namespace spirula {

struct ScalePair {
    double a[3] = {0, 0, 0}, b[3] = {0, 0, 0};   // file frame, dataset units
    double truth = 0;                            // the real distance, metres
};

struct ScaleFit {
    double s = 1.0;                 // least squares: sum(d m) / sum(m^2)
    std::vector<double> measured;   // |a b| per pair, dataset units
    std::vector<double> residual;   // s m - d per pair, metres
    double spread = 0;              // std of the per-pair scales over s; 0 for one pair
    double sigma = 0;               // spread / sqrt(n): the relative scale uncertainty
};

// Throws when no pair has a positive measured and true length.
ScaleFit fit_scale(const std::vector<ScalePair>& pairs);

struct RescaleOptions {
    bool apply = false;              // false: report what would change, write nothing
    std::vector<std::string> splats; // splat PLYs (a run's training frame)
    std::vector<std::string> meshes; // meshes in the file frame
    bool depth_metric = false;       // depth PNGs hold metric/mm values: multiply them
    std::string colmap_copy;         // non-empty: write a COLMAP copy here, leave `dir` alone
    // 16-bit PNG writer for the depth maps (app/DepthPng.h); the depths are
    // left alone without one.
    bool (*save_png16)(const std::string&, const uint16_t*, int, int) = nullptr;
};

struct RescaleReport {
    std::vector<std::string> rescaled;   // files written (or that would be)
    std::vector<std::string> skipped;    // artifacts left as they are, with why
};

// `sigma` is the relative uncertainty written to gauge.txt; 0 leaves it out.
void rescale_dataset(const std::string& dir, double s, double sigma,
                     const RescaleOptions& opt, RescaleReport& report);

}  // namespace spirula
