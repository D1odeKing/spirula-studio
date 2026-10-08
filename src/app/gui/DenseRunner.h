#pragma once

#include "dense/DenseConfig.h"
#include "app/gui/PrepProgress.h"

#include <atomic>

namespace gui {

struct DenseJob {
    bool enable = false, use_for_training = true;
    bool log_performance = false;   // --perf-dir; not a dense setting, so reuse ignores it
    spirula::dense::DenseConfig config;
};

std::string dense_availability();
bool dense_completed(const std::string& dataset);
bool run_dense_step(const DenseJob& job, const std::string& dataset, const std::string& images,
                    RunProgress& progress, const std::atomic<bool>& cancel, std::string& error);

}  // namespace gui
