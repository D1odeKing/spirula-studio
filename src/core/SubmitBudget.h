#pragma once

// How much work one GPU submission may carry, learned from how long the last
// ones took. A submit that runs past the driver's watchdog loses the device
// (VK_ERROR_DEVICE_LOST): 2 s under Windows TDR, and 2 s for amdgpu on Linux
// 7.0. Header-only: the SfM and inference runtimes link nothing else.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "core/Env.h"

namespace spirula {

class SubmitBudget {
public:
    // 0.25 s leaves 8x headroom under a 2 s watchdog for clocks that drop mid-run.
    // SS_SUBMIT_BUDGET_MS overrides it, for a device that still times out.
    // `prior_rate` is assumed until the first measurement replaces it.
    explicit SubmitBudget(double prior_rate = 0) : rate_(prior_rate) {
        target_ = 0.25;
        if (const char* v = env("SUBMIT_BUDGET_MS"))
            if (std::atof(v) > 0) target_ = std::atof(v) * 1e-3;
    }

    // Work the next submit may carry, in the caller's units; 0 until a submit
    // has been timed (and no prior), which means "send the smallest unit".
    double limit() const { return rate_ > 0 ? rate_ * target_ : 0; }
    double rate() const { return rate_; }
    double target() const { return target_; }
    bool measured() const { return measured_; }

    // limit() as a launch size in whole units: `first` before anything is
    // measured, never more than `most`, which keeps a fast GPU's launches as-is.
    int64_t chunk(int64_t first, int64_t most) const {
        if (limit() <= 0) return std::max<int64_t>(1, std::min(first, most));
        return std::max<int64_t>(1, std::min<int64_t>((int64_t)limit(), most));
    }

    // `solid_from` raises how long a sample must be to replace the rate, for a
    // caller whose shorter samples are unlike the launches it sizes.
    void record(double work, double seconds, double solid_from = 0) {
        if (work <= 0 || seconds <= 0) return;
        const double r = work / seconds;
        // A sample long enough to be mostly GPU work replaces the prior, and
        // replaces a faster estimate at once; anything else moves halfway in log
        // space, so a short submit's fixed latency cannot starve the next ones.
        const bool solid = seconds > std::max(target_ / 16, solid_from);
        if (rate_ <= 0 || (solid && (!measured_ || r < rate_))) rate_ = r;
        else rate_ = std::sqrt(rate_ * r);
        measured_ = measured_ || solid;
    }

private:
    double target_;
    double rate_;  // work per second
    bool measured_ = false;
};

}  // namespace spirula
