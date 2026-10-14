// frame_motion -- the adaptive frame plan (app/FrameMotion.h). Four things
// have gone wrong here and each is silent, so each is asserted: the count has
// to land on the budget, the gaps have to stay inside the rate bounds, a burst
// of motion must not swallow the budget it cannot spend, and a view change
// that comes back must not be paid for twice.

#include "app/FrameMotion.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "ok  " : "BAD ", what.c_str());
    if (!ok) g_failures++;
}

// One sample per source frame, so the plan is in the units the test states.
std::vector<int64_t> indices(size_t n) {
    std::vector<int64_t> v(n);
    for (size_t i = 0; i < n; i++) v[i] = (int64_t)i;
    return v;
}

void check_plan(const char* name, const std::vector<float>& cost, int skip,
                int window, float range, int64_t want) {
    const std::vector<int64_t> at = indices(cost.size());
    const std::vector<int64_t> plan = app::plan_by_motion(
        cost, at, (int64_t)cost.size(), skip, window, range, 0);
    const std::string tag(name);
    // Bisection lands on the budget or just under it; a tenth either way is
    // the granularity of a plan that can only place frames on samples.
    expect((int64_t)plan.size() <= want &&
               (int64_t)plan.size() >= want - want / 10 - 1,
           tag + ": " + std::to_string(plan.size()) + " frames for a budget of " +
               std::to_string(want));
    const int64_t min_gap = std::max<int64_t>(window, (int64_t)(skip / range));
    const int64_t max_gap = std::max<int64_t>(min_gap, (int64_t)(skip * range));
    int64_t tight = (int64_t)cost.size(), wide = 0;
    for (size_t i = 1; i < plan.size(); i++) {
        const int64_t gap = plan[i] - plan[i - 1];
        tight = std::min(tight, gap);
        wide = std::max(wide, gap);
    }
    expect(plan.size() < 2 || (tight >= min_gap && wide <= max_gap),
           tag + ": gaps " + std::to_string(tight) + ".." + std::to_string(wide) +
               " within " + std::to_string(min_gap) + ".." + std::to_string(max_gap));
}

}  // namespace

int main() {
    // Even motion: the plan should be the fixed schedule in all but name.
    {
        const std::vector<float> cost(600, 0.03f);
        check_plan("even", cost, 15, 3, 4.0f, 40);
        const std::vector<int64_t> plan = app::plan_by_motion(
            cost, indices(cost.size()), 600, 15, 3, 4.0f, 0);
        int64_t wide = 0, tight = 600;
        for (size_t i = 1; i < plan.size(); i++) {
            wide = std::max(wide, plan[i] - plan[i - 1]);
            tight = std::min(tight, plan[i] - plan[i - 1]);
        }
        expect(wide - tight <= 1, "even: the spacing stays even");
    }

    // Half the clip still, half moving: the moving half takes the frames.
    {
        std::vector<float> cost(600, 0.002f);
        for (size_t i = 300; i < 600; i++) cost[i] = 0.08f;
        check_plan("split", cost, 15, 3, 4.0f, 40);
        const std::vector<int64_t> plan = app::plan_by_motion(
            cost, indices(cost.size()), 600, 15, 3, 4.0f, 0);
        int first = 0;
        for (int64_t at : plan) first += at < 300 ? 1 : 0;
        expect(first * 2 < (int)plan.size() - first,
               "split: the moving half gets more than twice the frames");
    }

    // A burst one sample wide: its budget cannot be spent inside it, and the
    // rest of the clip must not be starved for it.
    {
        std::vector<float> cost(600, 0.01f);
        cost[200] = 40.0f;
        check_plan("burst", cost, 15, 3, 4.0f, 40);
    }

    // A tripod: nothing changes, so nothing justifies more than the slowest
    // rate the bounds allow.
    {
        const std::vector<float> cost(600, 0.0f);
        const std::vector<int64_t> plan = app::plan_by_motion(
            cost, indices(cost.size()), 600, 15, 3, 4.0f, 0);
        expect((int)plan.size() <= 600 / 60 + 1,
               "still: " + std::to_string(plan.size()) +
                   " frames, at most the slowest rate");
    }

    // The cap is a cap, not a target.
    {
        const std::vector<float> cost(600, 0.03f);
        const std::vector<int64_t> plan = app::plan_by_motion(
            cost, indices(cost.size()), 600, 15, 3, 4.0f, 12);
        expect((int)plan.size() <= 12, "cap: " + std::to_string(plan.size()) +
                                           " frames within a cap of 12");
        expect((int)plan.size() >= 10, "cap: the cap is nearly filled");
    }

    // Two clips on one rate: the budget is theirs together, so the one that
    // moves takes it off the one that does not.
    {
        std::vector<app::MotionPlanInput> in(2);
        for (int k = 0; k < 2; k++) {
            in[k].cost.assign(600, k == 0 ? 0.002f : 0.06f);
            in[k].ends = indices(600);
            in[k].frames = 600;
            in[k].skip = 15;
            in[k].window = 3;
        }
        const std::vector<std::vector<int64_t>> got = app::plan_by_motion(in, 4.0f);
        const int a = (int)got[0].size(), b = (int)got[1].size();
        expect(a + b <= 80 && a + b >= 70,
               "shared: " + std::to_string(a + b) + " frames for a budget of 80");
        expect(b > 2 * a, "shared: the clip that moves gets more than twice");
        // And a rate that is still each clip's own, whatever it spends.
        expect(a >= 600 / 60, "shared: the still clip keeps its slowest rate");
        expect(b <= 600 / 3 + 1, "shared: the moving clip keeps its fastest rate");
    }

    // One wrist that wobbles and one that pans, step for step the same amount
    // of frame turning over. Composed, the wobbler changes nothing and takes
    // the slowest rate; summed, it used to buy the average one.
    {
        std::vector<app::MotionPlanInput> in(2);
        for (int k = 0; k < 2; k++) {
            in[k].cost.assign(600, 0.06f);
            in[k].ends = indices(600);
            in[k].frames = 600;
            in[k].skip = 15;
            in[k].window = 3;
            in[k].step.resize(600);
            for (size_t i = 0; i < 600; i++) {
                const float d = (k == 0 && (i % 2)) ? -0.06f : 0.06f;
                in[k].step[i].turn[2] = d;
            }
        }
        const std::vector<std::vector<int64_t>> got = app::plan_by_motion(in, 4.0f);
        const int wobble = (int)got[0].size(), pan = (int)got[1].size();
        expect(wobble <= 600 / 60 + 1,
               "wobble: " + std::to_string(wobble) +
                   " frames, at most the slowest rate");
        expect(pan > 4 * wobble, "wobble: the pan takes what the wobble does not, " +
                                     std::to_string(pan) + " frames");
        expect(wobble + pan <= 80, "wobble: " + std::to_string(wobble + pan) +
                                       " frames within a budget of 80");
    }

    // A sphere's cost and a flat capture's are different numbers, so they do
    // not compete: each keeps the frames the fixed schedule would have given
    // it, however much more the flat one appears to move.
    {
        std::vector<app::MotionPlanInput> in(2);
        for (int k = 0; k < 2; k++) {
            in[k].cost.assign(600, k == 0 ? 0.004f : 0.06f);
            in[k].ends = indices(600);
            in[k].frames = 600;
            in[k].skip = 15;
            in[k].window = 3;
        }
        in[0].view = app::MotionView::Fisheye;
        in[0].out_fov = 3.4034f;
        const std::vector<std::vector<int64_t>> got = app::plan_by_motion(in, 4.0f);
        const int a = (int)got[0].size(), b = (int)got[1].size();
        expect(a >= 36 && a <= 40, "scales: the sphere keeps its own 40 frames, got " +
                                       std::to_string(a));
        expect(b >= 36 && b <= 40, "scales: the flat one keeps its own 40 frames, got " +
                                       std::to_string(b));
    }

    // A clip is budgeted on its own length, not the video's: 300 of 3000
    // frames measured at a skip of 15 is 20 kept frames, all inside it.
    {
        app::MotionPlanInput in;
        for (int64_t i = 1000; i < 1300; i++) {
            in.cost.push_back(0.02f);
            in.ends.push_back(i);
        }
        in.frames = 3000;
        in.span = 300;
        in.skip = 15;
        in.window = 3;
        const std::vector<std::vector<int64_t>> got = app::plan_by_motion({in}, 4.0f);
        const size_t n = got.empty() ? 0 : got[0].size();
        expect(n >= 18 && n <= 20, "clip: its own budget, got " + std::to_string(n));
        expect(n > 0 && got[0].front() >= 1000 && got[0].back() < 1300, "clip: inside it");
    }

    // Nothing to plan from.
    expect(app::plan_by_motion({}, {}, 0, 15, 3, 4.0f, 0).empty(),
           "empty: no samples, no plan");

    std::printf("%s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}
