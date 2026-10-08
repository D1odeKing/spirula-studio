#pragma once

// Time left in a counted step, from its rate over the last 90 seconds: recent
// enough to follow a change of pace (cached pairs, then inference), long enough
// to ride out one slow item. A new total or a count going back starts over.

#include <algorithm>
#include <cstdint>
#include <deque>
#include <utility>

namespace gui {

class StageEta {
public:
    void reset() { samples_.clear(); total_ = -1; }

    void update(double now, int64_t done, int64_t total) {
        if (total != total_ || (!samples_.empty() && done < samples_.back().second)) {
            samples_.clear();
            total_ = total;
        }
        if (samples_.empty() || done != samples_.back().second) samples_.emplace_back(now, done);
        while (samples_.size() > 2 && now - samples_[1].first > kWindow) samples_.pop_front();
        now_ = now;
    }

    // Negative until three counts span at least three seconds.
    double seconds() const {
        if (samples_.size() < 3 || total_ <= 0) return -1.0;
        const auto& [t0, d0] = samples_.front();
        const auto& [t1, d1] = samples_.back();
        if (t1 - t0 < 3.0 || d1 <= d0) return -1.0;
        const double per_item = (t1 - t0) / (double)(d1 - d0);
        // Time since the last count is spent on the next item, but a stall must not read as "done".
        return std::max(0.0, (double)(total_ - d1) * per_item - std::min(now_ - t1, per_item));
    }

private:
    static constexpr double kWindow = 90.0;
    std::deque<std::pair<double, int64_t>> samples_;
    int64_t total_ = -1;
    double now_ = 0.0;
};

}  // namespace gui
