#pragma once

#include "dense/Reconstruction.h"

namespace app {

struct DenseResult {
    std::string cloud, manifest;
    spirula::dense::ReconstructionStatistics statistics;
    uint64_t pairs = 0, cached_pairs = 0;
    double seconds = 0;
};

// `progress_dir`, when set, receives model.bin snapshots of the cloud (app/gui/SfmProgress.h);
// `perf_dir` the machine and phase counters tools/perf/perf_report.py reads.
DenseResult run_dense(const std::string& dataset, const spirula::dense::DenseConfig& config,
                       const spirula::dense::DenseProgress& progress = {}, const std::atomic<bool>* cancel = nullptr,
                       const std::string& progress_dir = {}, const std::string& perf_dir = {});

}  // namespace app
