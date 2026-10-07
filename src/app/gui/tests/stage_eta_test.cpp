// stage_eta -- the dense strip's time-left estimate (app/gui/StageEta.h).

#include "app/gui/StageEta.h"

#include <cmath>
#include <cstdio>
#include <string>

using gui::StageEta;

namespace {

int g_failures = 0;

void expect(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "ok  " : "BAD ", what.c_str());
    if (!ok) g_failures++;
}

bool is_close(double a, double b) { return std::fabs(a - b) < 1e-6; }

}  // namespace

int main() {
    StageEta eta;
    expect(eta.seconds() < 0, "unknown before any count");
    eta.update(0.0, 0, 100);
    eta.update(1.0, 1, 100);
    expect(eta.seconds() < 0, "unknown from two counts");
    eta.update(4.0, 4, 100);
    expect(is_close(eta.seconds(), 96.0), "one item per second leaves 96 s");

    eta.update(5.0, 4, 100);
    expect(is_close(eta.seconds(), 95.0), "time since the last count is subtracted");
    eta.update(60.0, 4, 100);
    expect(is_close(eta.seconds(), 95.0), "a stall subtracts at most one item, never reaching zero");

    StageEta pace;
    for (int i = 0; i <= 50; ++i) pace.update(i * 0.1, i, 1000);        // cached pairs: 10 per second
    for (int i = 1; i <= 100; ++i) pace.update(5.0 + i * 2.0, 50 + i, 1000);   // inference: one per 2 s
    expect(std::fabs(pace.seconds() - 850 * 2.0) < 1.0, "the 90 s window follows the slower pace");

    pace.update(300.0, 3, 20);
    expect(pace.seconds() < 0, "a new total starts over");
    pace.update(301.0, 4, 20); pace.update(304.0, 7, 20);
    expect(is_close(pace.seconds(), 13.0), "the new phase is estimated on its own");
    pace.update(305.0, 2, 20);
    expect(pace.seconds() < 0, "a count going back starts over");

    StageEta done;
    done.update(0.0, 0, 10); done.update(2.0, 5, 10); done.update(4.0, 10, 10);
    expect(is_close(done.seconds(), 0.0), "finished reads zero");

    std::printf(g_failures ? "FAIL %d\n" : "PASS stage time-left estimate\n", g_failures);
    return g_failures ? 1 : 0;
}
