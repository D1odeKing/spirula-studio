// dispatch_budgeted (VulkanPipelines.h): launches timed alone on the host now
// and then, and run as ranges of their grid when one would outlast the submit
// budget. docs/notes/gpu-submit-budget.md.

#include "backend/vulkan/VulkanPipelines.h"

#include "core/SubmitBudget.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <string>

namespace backend {
namespace vk {

namespace {

struct LaunchBudget {
    spirula::SubmitBudget budget;  // work units per second
    uint64_t since_sample = 0;     // launches since the last timed one
};

std::mutex g_budget_mutex;
std::map<std::string, LaunchBudget> g_budgets;
// The slowest rate any key of an entry has measured: the first estimate for
// another key of it (the bilateral grid's tile sizes), divided by
// kSiblingMargin since they differ.
std::map<std::string, double> g_entry_rates;
constexpr double kSiblingMargin = 4.0;

// A sample drains the queue first, so a short launch is re-timed only every
// this many: often enough to follow splat sizes and binning as they drift.
constexpr uint64_t kResampleEvery = 64;
// One predicted at a quarter of the budget or more is re-timed this often: its
// cost per unit moves as loss terms switch on (ppl_bwd: 2.7x after step 1 at
// 4946x3286), and against its length the drain costs nothing.
constexpr uint64_t kResampleLongEvery = 4;

using Clock = std::chrono::steady_clock;

}  // namespace

bool dispatch_budgeted(Stream stream, const char* entry_name,
                       const SpecList& spec, uint32_t groups_x,
                       uint32_t groups_y, uint32_t groups_z,
                       const void* params, uint32_t params_size, double work,
                       const char* key) {
    if (groups_x == 0 || groups_y == 0 || groups_z == 0) return true;
    StreamImpl* s = stream_impl(stream);
    if (!s)
        return dispatch(stream, entry_name, spec, groups_x, groups_y,
                        groups_z, params, params_size);
    Context& ctx = Context::get();

    std::lock_guard<std::mutex> lock(g_budget_mutex);
    LaunchBudget& b = g_budgets[key ? key : entry_name];
    const uint32_t grid[3] = {groups_x, groups_y, groups_z};
    const int axis = groups_x >= groups_y && groups_x >= groups_z ? 0
                     : groups_y >= groups_z                       ? 1
                                                                  : 2;
    const uint32_t n = grid[axis];
    if (!(work > 0)) work = (double)groups_x * groups_y * groups_z;
    const double per_slice = work / n;
    const double target = b.budget.target();

    // An entry's first launch runs whole: nothing yet says how to size it,
    // and a split one would build the vkCmdDispatchBase pipeline on every
    // device (8.7 s of one cold step on an RTX 3070) for nothing.
    const bool timed = b.budget.rate() > 0;
    double rate = b.budget.rate();
    if (!timed) {
        auto it = g_entry_rates.find(entry_name);
        if (it != g_entry_rates.end()) rate = it->second / kSiblingMargin;
    }
    const bool long_launch = rate > 0 && work / rate >= target / 4;
    const bool sample =
        !timed || ++b.since_sample >=
                      (long_launch ? kResampleLongEvery : kResampleEvery);
    if (sample) b.since_sample = 0;
    auto slices_at = [&](double r) -> uint32_t {
        if (r <= 0) return n;
        return (uint32_t)std::clamp(r * target / per_slice, 1.0, (double)n);
    };

    uint32_t chunk = slices_at(rate);
    // A sample times each range alone; otherwise what came before a launch
    // that has to be split is submitted on its own.
    if (sample) {
        if (!ctx.wait(flush_all_streams())) return false;
    } else if (chunk < n) {
        stream_flush(s);
    }
    // The ranges of one launch differ (sky rows, image borders), and the
    // slowest is the one a watchdog sees.
    double slow_work = 0, slow_sec = 0, all_work = 0, all_sec = 0;
    for (uint32_t done = 0; done < n;) {
        const uint32_t count = std::min(chunk, n - done);
        const double t = rate > 0 ? per_slice * count / rate : 0.0;
        if (!sample && s->open_seconds > 0 && s->open_seconds + t > target)
            stream_flush(s);
        const Clock::time_point t0 = Clock::now();
        bool ok;
        if (count == n) {
            ok = dispatch(stream, entry_name, spec, groups_x, groups_y,
                          groups_z, params, params_size);
        } else {
            uint32_t base[3] = {0, 0, 0};
            uint32_t range[3] = {groups_x, groups_y, groups_z};
            base[axis] = done;
            range[axis] = count;
            ok = dispatch_range(stream, entry_name, spec, base, range, params,
                                params_size);
        }
        if (!ok) return false;
        done += count;
        if (!sample) {
            s->open_seconds += t;
            continue;
        }
        if (!ctx.wait(stream_flush(s))) return false;
        const double w = per_slice * count;
        const double sec =
            std::chrono::duration<double>(Clock::now() - t0).count();
        if (sec <= 0) continue;
        all_work += w;
        all_sec += sec;
        const bool solid = sec > target / 16;  // as SubmitBudget::record
        if (solid && (slow_sec == 0 || w / sec < slow_work / slow_sec)) {
            slow_work = w;
            slow_sec = sec;
        }
        if (rate <= 0 || (solid && w / sec < rate)) rate = w / sec;
        chunk = slices_at(rate);
    }
    if (slow_sec > 0)
        b.budget.record(slow_work, slow_sec);
    else if (all_sec > 0)
        b.budget.record(all_work, all_sec);
    if (b.budget.rate() > 0) {
        auto [it, fresh] = g_entry_rates.emplace(entry_name, b.budget.rate());
        if (!fresh) it->second = std::min(it->second, b.budget.rate());
    }
    return true;
}

}  // namespace vk
}  // namespace backend
