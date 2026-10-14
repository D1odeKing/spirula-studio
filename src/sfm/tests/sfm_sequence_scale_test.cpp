// Scale breaks along a sequence (sfm/map/SequenceScale.h, D80): synthetic
// walks with a stretch at the wrong scale, a walker who stops, and a rig.
//
// Prints PASS/FAIL and returns 0/1. See docs/testing.md.
#include <cmath>
#include <cstdio>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "sfm/map/SequenceScale.h"
#include "sfm/tests/TestMain.h"

using namespace sfm;

static int fails = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        printf("  FAIL: %s\n", what);
        fails++;
    }
}

struct Walk {
    int positions = 120;
    int lenses = 1;
    double step = 1.0;
    int stop_from = -1, stop_to = -1;   // positions the walker stands still
    double pace = 0.2;                  // step varies by this fraction
    bool wall = false;                  // the scene closes in from 12 m to 2 m
    int hinge = -1;                     // positions before it shrunk ...
    double shrink = 1.0 / 9;            // ... by this, about the hinge
    bool hinge_side_later = true;       // the shrunk side registered last
};

struct Built {
    Reconstruction rec;
    SequenceTable seq;
    std::vector<uint64_t> tick;
};

// A walk along x with points 5-15 m around each position. Registered from the
// hinge outward, the way a mapper grows a sequence from where it joined.
static Built build(const Walk& w, uint32_t seed = 7) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(-1, 1);
    Built b;
    std::vector<Vec3> c(w.positions);
    double x = 0;
    for (int p = 0; p < w.positions; p++) {
        c[p] = {x, 0, 0};
        const bool stopped = p >= w.stop_from && p < w.stop_to;
        x += stopped ? 0.01 * w.step : w.step * (1 + w.pace * u(rng));
    }
    const int h = w.hinge >= 0 ? w.hinge : 0;
    const Vec3 H = c[h];
    auto place = [&](int p, const Vec3& v) {
        return (w.hinge >= 0 && p < w.hinge) ? H + (v - H) * w.shrink : v;
    };
    std::vector<std::string> names;
    uint64_t pid = 1;
    for (int p = 0; p < w.positions; p++)
        for (int l = 0; l < w.lenses; l++) {
            const uint32_t id = (uint32_t)names.size();
            char nm[64];
            snprintf(nm, sizeof nm, "vid/cam%d/%05d.jpg", l, p * 24);
            names.push_back(nm);
            Image im;
            im.id = id;
            im.registered = true;
            const Vec3 cc = place(p, c[p]);
            im.pose = {mat3Identity(), cc * -1.0};
            for (int k = 0; k < 40; k++) {
                Vec3 d{u(rng), u(rng), u(rng)};
                const double r = w.wall ? 12.0 - 10.0 * p / w.positions : 10.0;
                d = d.normalized() * (r * (1 + 0.5 * u(rng)));
                Point3D pt;
                pt.xyz = place(p, c[p] + d);
                b.rec.points3D[pid] = pt;
                im.point3D_ids.push_back(pid++);
            }
            b.rec.images[id] = im;
        }
    std::vector<SequenceDef> defs(1);
    for (int l = 0; l < w.lenses; l++) defs[0].members.push_back("vid/cam" + std::to_string(l));
    b.seq = buildSequenceTable(names, defs);
    // Ticks: the hinge's later side last, each side growing away from it.
    b.tick.assign(names.size(), 0);
    uint64_t t = 1;
    auto stamp = [&](int p) {
        for (int l = 0; l < w.lenses; l++) b.tick[(size_t)(p * w.lenses + l)] = t++;
    };
    const bool before_last = w.hinge_side_later;
    if (before_last) {
        for (int p = h; p < w.positions; p++) stamp(p);
        for (int p = h - 1; p >= 0; p--) stamp(p);
    } else {
        for (int p = h - 1; p >= 0; p--) stamp(p);
        for (int p = h; p < w.positions; p++) stamp(p);
    }
    return b;
}

static std::set<uint32_t> asSet(const std::vector<uint32_t>& v) { return {v.begin(), v.end()}; }

static void testShrunkStretch() {
    Walk w;
    w.hinge = 70;
    Built b = build(w);
    const auto br = findSequenceScaleBreaks(b.rec, b.seq, b.tick, {});
    check(br.size() == 1, "shrunk: one break");
    if (br.size() != 1) return;
    check(br[0].pos == 69, "shrunk: at the hinge, on the later side");
    check(std::fabs(br[0].step_ratio - 1.0 / 9) < 0.05, "shrunk: step ratio about 1/9");
    check(br[0].depth_ratio < 0.2, "shrunk: depth agrees");
    std::set<uint32_t> want;
    for (uint32_t i = 0; i < 70; i++) want.insert(i);
    check(asSet(br[0].drop) == want, "shrunk: drops every frame built on the hinge");
    printf("  shrunk stretch: break at %d, steps x%.3f, depth x%.3f, %zu to drop\n", br[0].pos,
           br[0].step_ratio, br[0].depth_ratio, br[0].drop.size());
}

static void testEarlierSideShrunk() {
    // The large side came later: it is the one built on the break.
    Walk w;
    w.hinge = 50;
    w.hinge_side_later = false;
    Built b = build(w);
    const auto br = findSequenceScaleBreaks(b.rec, b.seq, b.tick, {});
    check(br.size() == 1 && br[0].pos == 50 && br[0].step_ratio > 5,
          "later large side: flagged, ratio above 1");
    if (br.size() == 1)
        check(br[0].drop.size() == 70 && asSet(br[0].drop).count(50) && !asSet(br[0].drop).count(49),
              "later large side: drops positions 50 onward");
}

static void testStop() {
    // A long stop changes the steps but not how far away the scene is.
    Walk w;
    w.stop_from = 40;
    w.stop_to = 80;
    Built b = build(w);
    check(findSequenceScaleBreaks(b.rec, b.seq, b.tick, {}).empty(), "a stop is not a break");
}

static void testPlainWalk() {
    Walk w;
    for (uint32_t seed = 1; seed <= 5; seed++) {
        Built b = build(w, seed);
        check(findSequenceScaleBreaks(b.rec, b.seq, b.tick, {}).empty(), "a plain walk has none");
    }
}

static void testWallAndPace() {
    // The scene closing in at an even pace, and an uneven pace in open space.
    Walk w;
    w.wall = true;
    Built b = build(w);
    check(findSequenceScaleBreaks(b.rec, b.seq, b.tick, {}).empty(), "a wall drawing near: none");
    Walk v;
    v.pace = 0.5;
    for (uint32_t seed = 1; seed <= 5; seed++) {
        b = build(v, seed);
        check(findSequenceScaleBreaks(b.rec, b.seq, b.tick, {}).empty(), "an uneven pace: none");
    }
    v.hinge = 60;
    b = build(v, 3);
    check(findSequenceScaleBreaks(b.rec, b.seq, b.tick, {}).size() == 1,
          "an uneven pace still shows a break");
}

static void testMildChange() {
    // Twice the scale is under the default 3x: left alone.
    Walk w;
    w.hinge = 60;
    w.shrink = 0.5;
    Built b = build(w);
    check(findSequenceScaleBreaks(b.rec, b.seq, b.tick, {}).empty(), "2x is under the bar");
    SequenceScaleOptions o;
    o.ratio = 1.6;
    o.depth_ratio = 1.4;
    check(findSequenceScaleBreaks(b.rec, b.seq, b.tick, o).size() == 1, "... and over a lower one");
}

static void testRig() {
    Walk w;
    w.lenses = 2;
    w.hinge = 90;
    Built b = build(w);
    const auto br = findSequenceScaleBreaks(b.rec, b.seq, b.tick, {});
    check(br.size() == 1 && br[0].drop.size() == 180, "rig: both lenses of every frame dropped");
}

static void testRecent() {
    // A check near the newest registrations finds a break there and still
    // undoes everything built on it; one far from the break leaves it to the
    // final check over everything.
    Walk w;
    w.hinge = 70;
    Built b = build(w);
    std::vector<std::vector<int32_t>> close_by(1), away(1), none(1);
    for (int32_t p = 55; p < 75; p++) close_by[0].push_back(p);
    for (int32_t p = 0; p < 15; p++) away[0].push_back(p);
    const auto br = findSequenceScaleBreaks(b.rec, b.seq, b.tick, {}, &close_by);
    check(br.size() == 1 && br[0].drop.size() == 70, "recent near the break: all 70 undone");
    check(findSequenceScaleBreaks(b.rec, b.seq, b.tick, {}, &away).empty(),
          "recent far from the break: left alone");
    check(findSequenceScaleBreaks(b.rec, b.seq, b.tick, {}, &none).empty(),
          "no recent position: nothing checked");
}

static void testShortRunsAndOff() {
    // Too few stations on the short side to judge; and ratio 0 turns it off.
    Walk w;
    w.hinge = 5;
    Built b = build(w);
    check(findSequenceScaleBreaks(b.rec, b.seq, b.tick, {}).empty(), "too short a side: none");
    w.hinge = 60;
    b = build(w);
    SequenceScaleOptions off;
    off.ratio = 0;
    check(findSequenceScaleBreaks(b.rec, b.seq, b.tick, off).empty(), "ratio 0: off");
    // An unregistered gap longer than the window splits the walk: a scale
    // change across it is not this check's to judge.
    for (uint32_t i = 52; i < 68; i++) b.rec.images[i].registered = false;
    check(findSequenceScaleBreaks(b.rec, b.seq, b.tick, {}).empty(), "across a gap: none");
}

static int run(int, char**) {
    testShrunkStretch();
    testEarlierSideShrunk();
    testStop();
    testPlainWalk();
    testWallAndPace();
    testMildChange();
    testRig();
    testRecent();
    testShortRunsAndOff();
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}

int main(int argc, char** argv) { return sfmTestMain(argc, argv, run); }
