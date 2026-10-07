#pragma once

#include "dense/DenseConfig.h"
#include "app/gui/PrepProgress.h"

#include <atomic>

namespace gui {

struct DenseJob {
    bool enable = false, use_for_training = true;
    spirula::dense::DenseConfig config;
};

std::string dense_availability();
bool dense_completed(const std::string& dataset);
bool run_dense_step(const DenseJob& job, const std::string& dataset, const std::string& images,
                    RunProgress& progress, const std::atomic<bool>& cancel, std::string& error);

}  // namespace gui
