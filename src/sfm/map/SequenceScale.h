// Scale breaks along a sequence (D80): a frame registered too close to its
// neighbour gives the points it triangulates too little depth, every later
// frame registers against those, and the rest of the walk comes out at a
// fraction of its size while explaining its own matches perfectly. Seen on a
// 360 walk: 400 frames at 1/9 scale, hinged on one registration. Found by the
// two things such a break changes together: how far the camera moves per
// frame, and how far away the points it sees are. A walker who stops changes
// only the first. docs/notes/sfm-sequence-scale.md.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "sfm/core/Model.h"
#include "sfm/core/Pose.h"
#include "sfm/core/Sequence.h"

namespace sfm {

struct SequenceScaleOptions {
    double ratio = 3.0;       // step change that counts as a break; 0 checks nothing
    double depth_ratio = 2.0; // ... confirmed by the scene's depth changing this much
    int window = 8;           // registered positions compared on each side
    int min_points = 20;      // observations a position needs for its depth
};

struct SequenceScaleBreak {
    int32_t sequence = kNoSequence;
    int32_t pos = 0;             // first position on the side that came later
    double step_ratio = 1;       // later side's step over the earlier side's
    double depth_ratio = 1;      // ... and the same for depth
    std::vector<uint32_t> drop;  // images registered on the break, to undo
};

namespace seqscale_detail {

struct Station {
    int32_t pos = 0;
    Vec3 c;
    double depth = 0;
    uint64_t tick = 0;   // earliest registration among its images
    std::vector<uint32_t> images;
};

inline double medianOf(std::vector<double> v) {
    if (v.empty()) return 0;
    const size_t m = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + m, v.end());
    return v[m];
}

// Median distance from the camera to the points it observes.
inline double observedDepth(const Reconstruction& rec, const Image& im, const Vec3& c,
                            int& n) {
    std::vector<double> d;
    for (uint64_t id : im.point3D_ids) {
        if (id == kInvalidPoint3D) continue;
        auto it = rec.points3D.find(id);
        if (it == rec.points3D.end()) continue;
        d.push_back((it->second.xyz - c).norm());
    }
    n = (int)d.size();
    return medianOf(std::move(d));
}

// The registered positions of each sequence, in order. Depth, the costly
// part, only within `margin` of a `recent` position (all when null); the
// rest carry depth -1. A sequence with no recent position is left empty.
inline std::vector<std::vector<Station>> stations(
    const Reconstruction& rec, const SequenceTable& seq, const std::vector<uint64_t>& tick,
    int min_points, const std::vector<std::vector<int32_t>>* recent, int margin) {
    std::vector<std::vector<std::vector<uint32_t>>> at(seq.length.size());
    std::vector<std::vector<char>> deep(seq.length.size());
    for (uint32_t s = 0; s < seq.length.size(); s++) {
        const bool any = !recent || (s < recent->size() && !(*recent)[s].empty());
        if (!any) continue;
        at[s].resize(seq.length[s]);
        deep[s].assign(seq.length[s], recent ? 0 : 1);
        if (recent)
            for (int32_t p : (*recent)[s])
                for (int32_t q = std::max(0, p - margin);
                     q <= std::min((int32_t)seq.length[s] - 1, p + margin); q++)
                    deep[s][(size_t)q] = 1;
    }
    for (const auto& kv : rec.images) {
        if (!kv.second.registered || !seq.has(kv.first)) continue;
        const size_t s = (size_t)seq.seq[kv.first];
        if (!at[s].empty()) at[s][(size_t)seq.pos[kv.first]].push_back(kv.first);
    }
    std::vector<std::vector<Station>> out(at.size());
    for (size_t s = 0; s < at.size(); s++)
        for (size_t p = 0; p < at[s].size(); p++) {
            if (at[s][p].empty()) continue;
            Station st;
            st.pos = (int32_t)p;
            st.images = at[s][p];
            st.tick = UINT64_MAX;
            st.depth = -1;
            std::vector<double> depths;
            for (uint32_t img : st.images) {
                const Image& im = rec.images.at(img);
                const Vec3 c = cameraCenter(im.pose);
                if (img == st.images.front()) st.c = c;
                st.tick = std::min(st.tick, img < tick.size() ? tick[img] : 0);
                if (!deep[s][p]) continue;
                int n = 0;
                const double d = observedDepth(rec, im, c, n);
                if (n >= min_points) depths.push_back(d);
            }
            if (deep[s][p]) {
                if (depths.empty()) continue;
                st.depth = medianOf(depths);
            }
            out[s].push_back(std::move(st));
        }
    return out;
}

}  // namespace seqscale_detail

// Every break in the registered sequences, or only near `recent` positions
// (per sequence). `tick` orders registrations by image id: the side that came
// later is built on the break, and `drop` is its images from the break outward.
inline std::vector<SequenceScaleBreak> findSequenceScaleBreaks(
    const Reconstruction& rec, const SequenceTable& seq, const std::vector<uint64_t>& tick,
    const SequenceScaleOptions& opt,
    const std::vector<std::vector<int32_t>>* recent = nullptr) {
    using namespace seqscale_detail;
    std::vector<SequenceScaleBreak> out;
    if (!(opt.ratio > 1) || seq.empty()) return out;
    const int W = std::max(2, opt.window);
    const int G = W / 2;   // depth changes over a few frames, steps in one
    const double lr = std::log(opt.ratio), ld = std::log(std::max(1.0, opt.depth_ratio));
    const std::vector<std::vector<Station>> all = stations(rec, seq, tick, opt.min_points, recent, 3 * W + 2 * G);
    for (size_t s = 0; s < all.size(); s++) {
        const std::vector<Station>& st = all[s];
        // Runs of stations with no long unregistered gap: scale is only
        // carried from frame to frame inside one.
        size_t a = 0;
        while (a < st.size()) {
            size_t b = a + 1;
            while (b < st.size() && st[b].pos - st[b - 1].pos <= W) b++;
            const int n = (int)(b - a);
            // step[i]: from station a+i-1 to a+i, per position.
            std::vector<double> step(n, 0);
            for (int i = 1; i < n; i++)
                step[i] = (st[a + i].c - st[a + i - 1].c).norm() /
                          (double)(st[a + i].pos - st[a + i - 1].pos);
            struct Cand { int k; double ls, ld; };
            std::vector<Cand> cands;
            // A break between stations k-1 and k: steps compared on either
            // side of the one that crosses it, depths a few stations out.
            for (int k = W + G; k + W + G <= n; k++) {
                std::vector<double> sl(step.begin() + (k - W + 1), step.begin() + k);
                std::vector<double> sr(step.begin() + (k + 1), step.begin() + (k + W));
                std::vector<double> dl, dr;
                for (int i = k - G - W; i < k - G; i++) dl.push_back(st[a + i].depth);
                for (int i = k + G; i < k + G + W; i++) dr.push_back(st[a + i].depth);
                if (*std::min_element(dl.begin(), dl.end()) <= 0 ||
                    *std::min_element(dr.begin(), dr.end()) <= 0)
                    continue;   // depth not measured this far from anything recent
                const double ms_l = medianOf(sl), ms_r = medianOf(sr);
                const double md_l = medianOf(dl), md_r = medianOf(dr);
                if (!(ms_l > 0 && ms_r > 0 && md_l > 0 && md_r > 0)) continue;
                const double ls = std::log(ms_r / ms_l), lds = std::log(md_r / md_l);
                if (std::fabs(ls) < lr || std::fabs(lds) < ld || (ls > 0) != (lds > 0)) continue;
                cands.push_back({k, ls, lds});
            }
            // Windows near a break all clear the bar, so each run of candidates
            // is one break, placed where its steps split best into two levels.
            for (size_t c0 = 0; c0 < cands.size();) {
                size_t c1 = c0 + 1;
                while (c1 < cands.size() && cands[c1].k - cands[c1 - 1].k < W) c1++;
                const int lo = std::max(1, cands[c0].k - W + 1);
                const int hi = std::min(n - 1, cands[c1 - 1].k + W - 1);
                int k = cands[c0].k;
                double best = INFINITY;
                for (int sp = lo + 2; sp <= hi - 1; sp++) {
                    double m1 = 0, m2 = 0, cost = 0;
                    for (int i = lo; i < sp; i++) m1 += std::log(step[i]) / (sp - lo);
                    for (int i = sp; i <= hi; i++) m2 += std::log(step[i]) / (hi - sp + 1);
                    for (int i = lo; i <= hi; i++) {
                        const double e = std::log(step[i]) - (i < sp ? m1 : m2);
                        cost += e * e;
                    }
                    if (cost < best) best = cost, k = sp;
                }
                double ls = 0, lds = 0;
                for (size_t c = c0; c < c1; c++)
                    if (std::fabs(cands[c].ls) > std::fabs(ls)) ls = cands[c].ls, lds = cands[c].ld;
                c0 = c1;
                std::vector<uint64_t> tl, tr;
                for (int i = std::max(0, k - W); i < k; i++) tl.push_back(st[a + i].tick);
                for (int i = k; i < std::min(n, k + W); i++) tr.push_back(st[a + i].tick);
                std::sort(tl.begin(), tl.end());
                std::sort(tr.begin(), tr.end());
                const uint64_t ml = tl[tl.size() / 2], mr = tr[tr.size() / 2];
                const bool right_later = mr > ml;
                // Out from the break on the later side, past the few stations
                // at the hinge itself that belong to the earlier side, for as
                // long as they came nearer the later side's time than the other's.
                const uint64_t bar = std::min(ml, mr) + (std::max(ml, mr) - std::min(ml, mr)) / 2;
                SequenceScaleBreak br;
                br.sequence = (int32_t)s;
                br.step_ratio = std::exp(right_later ? ls : -ls);
                br.depth_ratio = std::exp(right_later ? lds : -lds);
                const int dir = right_later ? 1 : -1;
                int skipped = 0;
                for (int i = right_later ? k - G : k - 1 + G; i >= 0 && i < n; i += dir) {
                    if (st[a + i].tick <= bar) {
                        if (br.drop.empty() && skipped++ < 2 * G) continue;
                        break;
                    }
                    if (br.drop.empty()) br.pos = st[a + i].pos;
                    br.drop.insert(br.drop.end(), st[a + i].images.begin(), st[a + i].images.end());
                }
                if (!br.drop.empty()) out.push_back(std::move(br));
            }
            a = b;
        }
    }
    return out;
}

}  // namespace sfm
