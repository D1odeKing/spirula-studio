#include "sfm/Hybrid.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#include "core/Env.h"
#include "sfm/core/Cancel.h"
#include "sfm/core/Events.h"
#include "sfm/core/Log.h"
#include "sfm/core/Progress.h"
#include "sfm/core/Resume.h"
#include "sfm/feature/LearnedMatcher.h"
#include "sfm/feature/Verification.h"
#include "sfm/map/Merge.h"
#include "i18n/TimeFormat.h"
#include "i18n/catalog/Sfm.h"

namespace fs = std::filesystem;

namespace sfm {

namespace L = sfm::slog;
namespace M = spirula::i18n::msg::sfm;
using sfm::slog::Tag;
using spirula::i18n::format_duration;

namespace {

// Covisibility pairs on top of the learned frontend's verified ones: an image's
// partners sharing at least this many of the model's points, the best first.
constexpr uint32_t kMinShared = 15;
constexpr size_t kCovisiblePartners = 20;
// Longer tracks contribute their first elements only; a track of 200 would
// otherwise add 20k pair counts for one point.
constexpr size_t kTrackPairCap = 32;

// SIFT's own resolution at each --quality (SfmConfig.cpp applyPresets).
int siftImageSize(const std::string& quality) {
    if (quality == "low") return 1000;
    if (quality == "medium") return 1600;
    if (quality == "extreme") return 3200;
    return 2400;
}

double envNumber(const char* name, double fallback) {
    const char* v = spirula::env(name);
    return v && v[0] ? std::atof(v) : fallback;
}

double meanError(const Reconstruction& m, const std::vector<FeatureSet>& feats) {
    double mean = 0, median = 0;
    size_t n = 0;
    reprojStats(m, feats, mean, median, n);
    return mean;
}

size_t countPoints(const std::vector<Reconstruction>& models) {
    size_t n = 0;
    for (const Reconstruction& m : models) n += m.points3D.size();
    return n;
}

// The pairs to match: every pair of images one model registered that the learned
// frontend verified or that share its points. `owner` is the first model holding
// both, whose poses the epipolar gate uses.
std::vector<std::pair<uint32_t, uint32_t>> hybridPairs(const std::vector<Reconstruction>& models,
                                                       const MatchesDatabase& db,
                                                       std::vector<int>& owner) {
    const size_t n = db.images.size();
    std::map<std::pair<uint32_t, uint32_t>, int> chosen;
    for (size_t k = 0; k < models.size(); k++) {
        const Reconstruction& m = models[k];
        if (m.numRegistered() < 2) continue;
        std::vector<char> in(n, 0);
        for (const auto& kv : m.images)
            if (kv.second.registered && kv.first < n) in[kv.first] = 1;
        for (const TwoViewMatches& t : db.pairs)
            if (in[t.image1] && in[t.image2])
                chosen.emplace(std::minmax(t.image1, t.image2), (int)k);
        std::unordered_map<uint64_t, uint32_t> shared;
        for (const auto& kv : m.points3D) {
            const auto& tr = kv.second.track;
            const size_t len = std::min(tr.size(), kTrackPairCap);
            for (size_t a = 0; a < len; a++)
                for (size_t b = a + 1; b < len; b++) {
                    const uint32_t i = std::min(tr[a].image_id, tr[b].image_id);
                    const uint32_t j = std::max(tr[a].image_id, tr[b].image_id);
                    if (i != j) shared[((uint64_t)i << 32) | j]++;
                }
        }
        std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> partners;
        for (const auto& kv : shared) {
            if (kv.second < kMinShared) continue;
            const uint32_t i = (uint32_t)(kv.first >> 32), j = (uint32_t)kv.first;
            partners[i].emplace_back(kv.second, j);
            partners[j].emplace_back(kv.second, i);
        }
        for (auto& kv : partners) {
            auto& v = kv.second;
            std::sort(v.rbegin(), v.rend());
            if (v.size() > kCovisiblePartners) v.resize(kCovisiblePartners);
            for (const auto& p : v) chosen.emplace(std::minmax(kv.first, p.second), (int)k);
        }
    }
    std::vector<std::pair<uint32_t, uint32_t>> out;
    owner.clear();
    for (const auto& kv : chosen) {
        out.push_back(kv.first);
        owner.push_back(kv.second);
    }
    return out;
}

// Runs `worker` on `threads` threads (0: one per core), no more than `jobs`.
template <class F>
void runWorkers(F& worker, int threads, size_t jobs) {
    const unsigned hc = std::thread::hardware_concurrency();
    size_t nt = threads > 0 ? (size_t)threads : (size_t)std::max(1u, hc);
    nt = std::max<size_t>(1, std::min(nt, jobs));
    if (nt == 1) return worker();
    std::vector<std::thread> pool;
    for (size_t t = 0; t < nt; t++) pool.emplace_back([&] { worker(); });
    for (std::thread& t : pool) t.join();
}

// Every keypoint's bearing under the camera model k gives image i, for each
// (k, i) in `keys`.
using BearingMap = std::map<std::pair<int, uint32_t>, std::vector<Vec3>>;

BearingMap modelBearings(const std::vector<Reconstruction>& models,
                         const std::vector<FeatureSet>& feats,
                         const std::set<std::pair<int, uint32_t>>& keys, int threads) {
    BearingMap out;
    std::vector<BearingMap::iterator> slots;
    for (const auto& key : keys) slots.push_back(out.emplace(key, std::vector<Vec3>()).first);
    std::atomic<size_t> next{0};
    auto worker = [&] {
        for (size_t s = next++; s < slots.size(); s = next++) {
            const int k = slots[s]->first.first;
            const uint32_t img = slots[s]->first.second;
            const Reconstruction& rec = models[k];
            const Camera& cam = rec.cameras.at(rec.images.at(img).camera_id);
            std::vector<Vec3>& v = slots[s]->second;
            v.resize(feats[img].count());
            for (uint32_t f = 0; f < feats[img].count(); f++) {
                const Keypoint& kp = feats[img].keypoints[f];
                v[f] = cam.bearing({kp.x, kp.y}).normalized();
            }
        }
    };
    runWorkers(worker, threads, slots.size());
    return out;
}

// The pose of j relative to i in model k (x_j = R x_i + t), and the epipolar
// tolerance as a sine, from the coarser of the two images' pixels.
struct PairGeometry {
    Mat3 R;
    Vec3 t;
    double tol = 0;
    bool rotation_only = false;
};

PairGeometry pairGeometry(const Reconstruction& rec, const std::vector<FeatureSet>& feats,
                          uint32_t i, uint32_t j, double tol_px) {
    const Image& a = rec.images.at(i);
    const Image& b = rec.images.at(j);
    const double fa = std::max(1e-6, rec.cameras.at(a.camera_id).focal());
    const double fb = std::max(1e-6, rec.cameras.at(b.camera_id).focal());
    PairGeometry g;
    g.R = mul(b.pose.R, transpose(a.pose.R));
    g.t = b.pose.t - mul(g.R, a.pose.t);
    const double ta = tol_px * feats[i].pixelScale() / fa;
    const double tb = tol_px * feats[j].pixelScale() / fb;
    g.tol = std::sin(std::min(0.5, std::max(ta, tb)));
    const double cam_dist = std::max(cameraCenter(a.pose).norm(), cameraCenter(b.pose).norm());
    g.rotation_only = !(g.t.norm() > 1e-9 * std::max(1.0, cam_dist));
    return g;
}

// Keeps the putative matches within the tolerance of their epipolar plane,
// measured on bearings so an equirectangular image is gated like a pinhole one.
void epipolarGate(const PairGeometry& g, const std::vector<Vec3>& ba,
                  const std::vector<Vec3>& bb, std::vector<FeatureMatch>& m) {
    size_t w = 0;
    for (const FeatureMatch& fm : m) {
        const Vec3 ra = mul(g.R, ba[fm.idx1]);
        const Vec3& rb = bb[fm.idx2];
        double e;
        if (g.rotation_only) {
            e = (ra - rb).norm();
        } else {
            const Vec3 nrm = g.t.cross(ra);
            const double nn = nrm.norm();
            e = nn > 1e-12 ? std::fabs(nrm.dot(rb)) / nn : 0.0;
        }
        if (e <= g.tol) m[w++] = fm;
    }
    m.resize(w);
}

int distanceU8(const uint8_t* a, const uint8_t* b, uint32_t dim) {
    int d = 0;
    for (uint32_t k = 0; k < dim; k++) {
        const int x = (int)a[k] - (int)b[k];
        d += x * x;
    }
    return d;
}

// Guided search: each keypoint of i is compared only with the keypoints of j on
// the arc its ray sweeps in j, epipole to vanishing point, widened by the
// tolerance. The ratio test runs over those alone; `mutual` runs it from j's side
// too and keeps only mutual nearest neighbours, else one match per j keypoint.
void bandMatch(const PairGeometry& g, const std::vector<Vec3>& ba, const std::vector<Vec3>& bb,
               const FeatureSet& fa, const FeatureSet& fb, double ratio, bool mutual,
               size_t cap, std::vector<FeatureMatch>& out, uint64_t& putative) {
    out.clear();
    if (g.rotation_only || fa.dim != fb.dim || fa.dtype != DType::U8 || fb.dtype != DType::U8)
        return;
    const Vec3 e = g.t.normalized();
    const Vec3 u = (std::fabs(e.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0}).cross(e).normalized();
    const Vec3 v = e.cross(u);
    // j's keypoints by azimuth about the baseline, in rings of distance from the
    // epipole: the farther the ring, the narrower the azimuth window a band needs.
    static const double kRing[] = {0.5, 0.2, 0.08, 0.03, 0.0};
    constexpr int kRings = 5;
    std::vector<std::pair<float, uint32_t>> ring[kRings];
    for (uint32_t q = 0; q < (uint32_t)bb.size(); q++) {
        const Vec3& b = bb[q];
        const double s = (b - e * b.dot(e)).norm();
        int r = 0;
        while (s < kRing[r]) r++;
        ring[r].emplace_back((float)std::atan2(b.dot(v), b.dot(u)), q);
    }
    for (auto& r : ring) std::sort(r.begin(), r.end());
    const uint32_t dim = fa.dim;
    const uint8_t* da = fa.descData();
    const uint8_t* db = fb.descData();
    const double ratio2 = ratio * ratio;
    const double slack = std::asin(std::min(1.0, g.tol));
    std::vector<std::pair<int, uint32_t>> best(bb.size(), {INT_MAX, 0});
    std::vector<int> second(bb.size(), INT_MAX);
    struct Found {
        uint32_t p, q;
        int d;
    };
    std::vector<Found> found;
    for (uint32_t p = 0; p < (uint32_t)ba.size(); p++) {
        const Vec3 r = mul(g.R, ba[p]);
        const Vec3 rp = r - e * r.dot(e);
        const double sr = rp.norm();
        if (sr < 0.02) continue;
        const Vec3 rh = rp * (1.0 / sr);
        const Vec3 nrm = e.cross(r) * (1.0 / sr);
        const double phi = std::atan2(r.dot(v), r.dot(u));
        const double max_angle = std::acos(std::max(-1.0, std::min(1.0, r.dot(e)))) + slack;
        const double min_cos = max_angle >= M_PI ? -2.0 : std::cos(max_angle);
        const uint8_t* dp = da + (size_t)p * dim;
        int d1 = INT_MAX, d2 = INT_MAX;
        uint32_t q1 = 0;
        auto test = [&](uint32_t q) {
            const Vec3& b = bb[q];
            if (std::fabs(nrm.dot(b)) > g.tol || b.dot(e) < min_cos || b.dot(rh) < -g.tol) return;
            const int d = distanceU8(dp, db + (size_t)q * dim, dim);
            if (d < best[q].first) {
                second[q] = best[q].first;
                best[q] = {d, p};
            } else if (d < second[q]) {
                second[q] = d;
            }
            if (d < d1) {
                d2 = d1;
                d1 = d;
                q1 = q;
            } else if (d < d2) {
                d2 = d;
            }
        };
        for (int k = 0; k < kRings; k++) {
            const auto& rg = ring[k];
            if (rg.empty()) continue;
            const double w = kRing[k] > 0 ? std::asin(std::min(1.0, g.tol / kRing[k])) : M_PI;
            if (w >= 0.5 * M_PI) {
                for (const auto& x : rg) test(x.second);
                continue;
            }
            auto scan = [&](double lo, double hi) {
                auto it = std::lower_bound(rg.begin(), rg.end(), std::make_pair((float)lo, 0u));
                for (; it != rg.end() && it->first <= hi; ++it) test(it->second);
            };
            scan(std::max(-M_PI, phi - w), std::min(M_PI, phi + w));
            if (phi - w < -M_PI) scan(phi - w + 2 * M_PI, M_PI);
            if (phi + w > M_PI) scan(-M_PI, phi + w - 2 * M_PI);
        }
        if (d1 == INT_MAX || (d2 != INT_MAX && (double)d1 >= ratio2 * d2)) continue;
        putative++;
        found.push_back({p, q1, d1});
    }
    // Without `mutual`, j's nearest is taken among the accepted only.
    if (!mutual) {
        std::fill(best.begin(), best.end(), std::make_pair(INT_MAX, 0u));
        for (const Found& f : found)
            if (f.d < best[f.q].first) best[f.q] = {f.d, f.p};
    }
    std::vector<float> dist;
    for (const Found& f : found) {
        if (best[f.q].second != f.p || best[f.q].first != f.d) continue;
        if (mutual && second[f.q] != INT_MAX && (double)f.d >= ratio2 * second[f.q]) continue;
        out.push_back({f.p, f.q});
        dist.push_back((float)f.d);
    }
    keepClosest(out, dist, cap);
}

// `src`'s registered images that `dst` lost, put back at their `src` pose carried
// into `dst`'s frame, with no observations. Returns how many.
uint32_t restoreLost(const Reconstruction& src, Reconstruction& dst,
                     const std::vector<FeatureSet>& feats) {
    std::vector<Pose> from, to;
    std::vector<uint32_t> lost;
    for (const auto& kv : src.images) {
        if (!kv.second.registered) continue;
        auto it = dst.images.find(kv.first);
        if (it != dst.images.end() && it->second.registered) {
            from.push_back(kv.second.pose);
            to.push_back(it->second.pose);
        } else {
            lost.push_back(kv.first);
        }
    }
    if (lost.empty()) return 0;
    Sim3 T;
    if (!from.empty() && !estimateSim3FromPoses(from, to, T)) return 0;
    for (uint32_t id : lost) {
        const Image& s = src.images.at(id);
        Image im;
        im.id = id;
        im.camera_id = s.camera_id;
        im.name = s.name;
        im.exif_orientation = s.exif_orientation;
        im.pose = from.empty() ? s.pose : transformPose(T, s.pose);
        im.registered = true;
        im.points2D.resize(feats[id].count());
        for (uint32_t f = 0; f < feats[id].count(); f++)
            im.points2D[f] = {feats[id].keypoints[f].x, feats[id].keypoints[f].y};
        im.point3D_ids.assign(feats[id].count(), kInvalidPoint3D);
        if (!dst.cameras.count(s.camera_id)) dst.cameras[s.camera_id] = src.cameras.at(s.camera_id);
        dst.images[id] = std::move(im);
    }
    return (uint32_t)lost.size();
}

// The learned frontend's tracks as well, triangulated against the SIFT
// solve's poses without moving them: each image's rows become SIFT's followed
// by the learned ones, and the two families never share a track.
void addLearnedPoints(std::vector<Reconstruction>& models, std::vector<FeatureSet>& sf,
                      MatchesDatabase& sdb, const std::vector<FeatureSet>& lf,
                      const MatchesDatabase& ldb, const MapperOptions& mo,
                      const std::vector<uint32_t>& cam_ids, const RigTable& rigs,
                      const SequenceTable& seqs, HybridStats& st) {
    const size_t n = sf.size();
    std::vector<uint32_t> offset(n);
    for (size_t i = 0; i < n; i++) {
        FeatureSet& f = sf[i];
        offset[i] = f.count();
        const bool colors = f.hasColors() && lf[i].hasColors();
        f.keypoints.insert(f.keypoints.end(), lf[i].keypoints.begin(), lf[i].keypoints.end());
        if (colors)
            f.colors.insert(f.colors.end(), lf[i].colors.begin(), lf[i].colors.end());
        else
            f.colors.clear();
        sdb.images[i].num_features = f.count();
    }
    for (const TwoViewMatches& t : ldb.pairs) {
        std::vector<FeatureMatch> m;
        m.reserve(t.matches.size());
        for (const FeatureMatch& fm : t.matches)
            m.push_back({fm.idx1 + offset[t.image1], fm.idx2 + offset[t.image2]});
        sdb.pairs.push_back({t.image1, t.image2, t.config, MatchList(std::move(m))});
    }
    Mapper mapper(sdb, sf, mo, cam_ids, &rigs, &seqs, nullptr);
    for (size_t k = 0; k < models.size(); k++) {
        Reconstruction& m = models[k];
        if (m.numRegistered() < 2) continue;
        for (auto& kv : m.images) {
            Image& im = kv.second;
            const FeatureSet& f = sf[kv.first];
            for (uint32_t r = offset[kv.first]; r < f.count(); r++)
                im.points2D.push_back({f.keypoints[r].x, f.keypoints[r].y});
            im.point3D_ids.resize(f.count(), kInvalidPoint3D);
        }
        const size_t before = m.points3D.size();
        m = mapper.triangulateFixed(m);
        if (k < st.models.size()) {
            st.models[k].points_learned = m.points3D.size() - std::min(before, m.points3D.size());
            st.models[k].error_combined = meanError(m, sf);
        }
        slog::diag(Tag::Map, "[hybrid] model %zu: %zu SIFT points, %zu with the learned tracks",
                   k, before, m.points3D.size());
    }
}

}  // namespace

bool hybridSift(std::vector<Reconstruction>& models, std::vector<FeatureSet>& feats,
                MatchesDatabase& db, const SfmConfig& cfg, const VerifyCalibration& calib,
                const RigTable& rigs, const SequenceTable& seqs, const std::string& image_dir,
                const fs::path& workspace, HybridStats& st) {
    const bool verbose = !cfg.quiet;
    const double t0 = now();
    st = HybridStats();
    try {
        const size_t n = db.images.size();
        std::vector<char> registered(n, 0);
        for (const Reconstruction& m : models)
            if (m.numRegistered() >= 2)
                for (const auto& kv : m.images)
                    if (kv.second.registered && kv.first < n) registered[kv.first] = 1;
        std::set<std::string> only;
        for (size_t i = 0; i < n; i++)
            if (registered[i]) only.insert(db.images[i].name);
        if (only.size() < 2) throw std::runtime_error("fewer than two registered images");

        SfmConfig sc = cfg;
        sc.features = "sift";
        sc.matcher = "bruteforce";
        sc.max_image_size = (int)envNumber("SFM_HYBRID_IMAGE_SIZE", siftImageSize(cfg.quality));
        // gate: the global nearest neighbour, kept near its epipolar plane;
        // band: the nearest neighbour within that band; none: unguided.
        const char* mv = spirula::env("SFM_HYBRID_MATCH");
        std::string mode = mv && mv[0] ? mv : "band";
        if (spirula::env("SFM_HYBRID_GUIDED") && !spirula::env_on("SFM_HYBRID_GUIDED"))
            mode = "none";
        if (mode != "gate" && mode != "none") mode = "band";
        const bool guided = mode != "none";
        sc.match = MatchOptions();
        sc.match.device = cfg.match.device;
        sc.match.device_selector = cfg.match.device_selector;
        sc.match.batch_pairs = cfg.match.batch_pairs;
        sc.match.descriptor_budget_bytes = cfg.match.descriptor_budget_bytes;
        sc.match.max_num_matches = cfg.match.max_num_matches;
        // The geometry does the rejecting the ratio test and the cross-check would.
        // On a 678-image interior (a7iii_inside), a 2 px band at 0.9 gave three
        // times the gate's points at 1.33 px against 1.74; 0.8 lost 39% for nothing.
        sc.match.max_ratio =
            (float)envNumber("SFM_HYBRID_RATIO", mode == "gate" ? 0.95 : mode == "band" ? 0.9 : 0.8);
        sc.match.cross_check = envNumber("SFM_HYBRID_CROSS_CHECK", guided ? 0 : 1) != 0;
        sc.sift.peak_threshold *= envNumber("SFM_HYBRID_PEAK_SCALE", 1.0);
        sc.sift.max_num_features =
            (int)envNumber("SFM_HYBRID_FEATURES", cfg.sift.max_num_features);
        st.images = only.size();
        st.image_size = sc.max_image_size;
        st.features = sc.sift.max_num_features;

        // ---- extract ----
        const fs::path siftdir = workspace / "features.sift";
        const fs::path sigfile = resume::dir(workspace.string()) / "hybrid-extract.sig";
        const std::string sig = stageSignature(sc, CMD_EXTRACT) + resume::kSignedImages +
                                image_dir + "\n";
        std::error_code ec;
        const bool reuse = cfg.reuse && resume::recorded(sigfile) == sig;
        if (!reuse) fs::remove_all(siftdir, ec);
        if (cfg.reuse) resume::store(sigfile, sig);
        if (verbose)
            L::out(Tag::Extract, M::hybrid_extract,
                   {(long long)st.images, (long long)st.image_size, (long long)st.features});
        ExtractStats est;
        if (extractDirectory(image_dir, siftdir, sc, est, reuse, nullptr, &only))
            throw std::runtime_error(spirula::i18n::format(M::progressive_detect_stopped, {}));
        cancel::check();

        std::vector<FeatureSet> sf(n);
        for (size_t i = 0; i < n; i++) {
            if (registered[i]) {
                sf[i] = readFeatures((siftdir / (db.images[i].name + ".bin")).string(), true, true);
            } else {
                sf[i].width = feats[i].width;
                sf[i].height = feats[i].height;
                sf[i].extract_width = feats[i].extract_width;
                sf[i].extract_height = feats[i].extract_height;
                sf[i].exif_focal = feats[i].exif_focal;
                sf[i].exif_camera = feats[i].exif_camera;
                sf[i].exif_orientation = feats[i].exif_orientation;
            }
        }

        // ---- match under the learned poses, and verify ----
        std::vector<int> owner;
        const std::vector<std::pair<uint32_t, uint32_t>> pairs = hybridPairs(models, db, owner);
        st.pairs = pairs.size();
        std::set<std::pair<int, uint32_t>> keys;
        for (size_t p = 0; p < pairs.size(); p++)
            if (guided) {
                keys.emplace(owner[p], pairs[p].first);
                keys.emplace(owner[p], pairs[p].second);
            }
        BearingMap bearings = modelBearings(models, sf, keys, cfg.threads);
        const double tol_px =
            envNumber("SFM_HYBRID_EPIPOLAR_PX", (mode == "band" ? 2.0 / 3.0 : 1.5) * cfg.max_error);
        auto geometry = [&](size_t p) {
            return pairGeometry(models[owner[p]], sf, pairs[p].first, pairs[p].second, tol_px);
        };
        auto pairBearings = [&](size_t p, bool second) -> const std::vector<Vec3>& {
            return bearings.at({owner[p], second ? pairs[p].second : pairs[p].first});
        };
        std::unique_ptr<IFeatureMatcher> matcher;
        if (mode != "band")
            matcher = createFeatureMatcher(sc.matcher, sc.match, sc.lightglue, sc.loma_match);
        const bool mutual = envNumber("SFM_HYBRID_BAND_MUTUAL", 1) != 0;
        uint64_t putative = 0, kept = 0;
        auto matchFn = [&](size_t b, size_t e, std::vector<std::vector<FeatureMatch>>& out) {
            if (mode == "band") {
                out.assign(e - b, {});
                std::atomic<size_t> next{b};
                std::atomic<uint64_t> found{0};
                auto worker = [&] {
                    uint64_t mine = 0;
                    for (size_t p = next++; p < e; p = next++)
                        bandMatch(geometry(p), pairBearings(p, false), pairBearings(p, true),
                                  sf[pairs[p].first], sf[pairs[p].second], sc.match.max_ratio,
                                  mutual, sc.match.max_num_matches, out[p - b], mine);
                    found += mine;
                };
                runWorkers(worker, cfg.threads, e - b);
                putative += found;
                for (const auto& m : out) kept += m.size();
                return;
            }
            matcher->matchBatch(sf, pairs, b, e, out);
            for (size_t p = b; p < e; p++) {
                std::vector<FeatureMatch>& m = out[p - b];
                putative += m.size();
                if (guided)
                    epipolarGate(geometry(p), pairBearings(p, false), pairBearings(p, true), m);
                kept += m.size();
            }
        };
        VerificationOptions vo;
        vo.two_view = cfg.twoview;
        vo.two_view.ransac.max_error = cfg.max_error;
        vo.num_threads = cfg.threads;
        vo.match_batch_pairs = mode == "band" ? std::max(sc.match.batch_pairs, 256)
                                              : sc.match.batch_pairs;
        BearingCache bc;
        std::vector<Camera> percam;
        const CameraSetup& cs = calib.cameras;
        if (cs.anyWide()) {
            bc = precomputeBearings(sf, perImageCameras(cs, n), !cs.mixed(), cfg.threads);
            vo.bearings = &bc;
        }
        if (calib.priors && cfg.sensor_verify && calib.priors->anyRotation()) {
            percam = perImageCameras(cs, n);
            vo.priors = calib.priors.get();
            vo.cameras = &percam;
        }
        progress::live_matches_end();
        progress::begin_matching((uint32_t)n, pairs);
        events::stage_begin(Stage::Match, (int64_t)pairs.size());
        std::vector<TwoViewMatches> verified = verifyPairs(sf, pairs, matchFn, vo);
        events::stage_end(Stage::Match);
        matcher.reset();
        BearingMap().swap(bearings);
        for (FeatureSet& f : sf) f.dropDescriptors();
        st.putative = putative;
        st.guided_kept = kept;
        st.verified = verified.size();
        for (const TwoViewMatches& t : verified) st.inliers += t.matches.size();
        if (verbose)
            L::out(Tag::Match, M::hybrid_matched,
                   {(long long)st.verified, (long long)st.pairs, (long long)st.inliers,
                    format_duration(now() - t0)});
        if (verbose && guided)
            L::diag(Tag::Match, "[hybrid] %s matching kept %llu of %llu putative matches",
                    mode.c_str(), (unsigned long long)kept, (unsigned long long)putative);

        MatchesDatabase sdb;
        sdb.images = db.images;
        for (size_t i = 0; i < n; i++) sdb.images[i].num_features = sf[i].count();
        sdb.pairs = std::move(verified);
        sdb.cameras = db.cameras;
        sdb.camera_ids = db.camera_ids;
        sdb.focal_prior = db.focal_prior;
        sdb.focal_measured = db.focal_measured;

        // ---- triangulate against the learned poses, then bundle-adjust ----
        CameraSetup cs2 = cs;
        std::map<uint32_t, std::vector<double>> scales;
        for (size_t i = 0; i < n && i < cs2.ids.size(); i++)
            if (registered[i]) scales[cs2.ids[i]].push_back(sf[i].pixelScale());
        for (auto& kv : scales) {
            auto c = cs2.cameras.find(kv.first);
            if (c == cs2.cameras.end()) continue;
            std::vector<double>& v = kv.second;
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            c->second.pixel_scale = std::max(1.0, v[v.size() / 2]);
        }
        MapperOptions mo = cfg.mapper;
        mo.initial_cameras = cs2.cameras;
        mo.known_focal_cameras = cs2.focal_known;
        mo.given_focal_cameras = cs2.focal_given;
        mo.measured_focal_cameras = cs2.focal_measured;
        std::vector<Reconstruction> out;
        {
            Mapper mapper(sdb, sf, mo, cs2.ids, &rigs, &seqs,
                          cfg.sensor_map ? calib.positionPriors() : nullptr);
            for (const Reconstruction& m : models) {
                cancel::check();
                Reconstruction bare;
                bare.cameras = m.cameras;
                bare.rigs = m.rigs;
                bare.rig_detached = m.rig_detached;
                for (const auto& kv : m.images) {
                    if (!kv.second.registered) continue;
                    Image im = kv.second.withoutPoints2D();
                    const FeatureSet& f = sf[kv.first];
                    im.points2D.resize(f.count());
                    for (uint32_t k = 0; k < f.count(); k++)
                        im.points2D[k] = {f.keypoints[k].x, f.keypoints[k].y};
                    im.point3D_ids.assign(f.count(), kInvalidPoint3D);
                    bare.images[kv.first] = std::move(im);
                }
                out.push_back(m.numRegistered() >= 2 ? mapper.retriangulate(bare) : bare);
            }
            double secs = 0;
            out = finishModels(mapper, std::move(out), cfg, verbose, secs);
        }

        for (size_t k = 0; k < models.size(); k++) {
            HybridModelStats ms;
            ms.images = models[k].numRegistered();
            ms.points_before = models[k].points3D.size();
            ms.error_before = meanError(models[k], feats);
            ms.sift = out[k].numRegistered();
            ms.restored = restoreLost(models[k], out[k], sf);
            ms.points_after = out[k].points3D.size();
            ms.error_after = meanError(out[k], sf);
            st.models.push_back(ms);
        }
        const HybridModelStats& main = st.models.front();
        if (out.front().points3D.empty() || 2 * main.sift < main.images)
            throw std::runtime_error("SIFT kept " + std::to_string(main.sift) + " of " +
                                     std::to_string(main.images) +
                                     " images of the largest model");
        st.seconds = now() - t0;
        if (verbose)
            for (size_t k = 0; k < st.models.size(); k++) {
                const HybridModelStats& ms = st.models[k];
                L::out(Tag::Map, M::hybrid_model,
                       {(long long)k, (long long)ms.images, (long long)ms.sift,
                        (long long)ms.restored, (long long)ms.points_before,
                        (long long)ms.points_after, L::num(ms.error_before, 2),
                        L::num(ms.error_after, 2)});
            }
        if (spirula::env_on("SFM_HYBRID_LEARNED_POINTS"))
            addLearnedPoints(out, sf, sdb, feats, db, mo, cs2.ids, rigs, seqs, st);
        models = std::move(out);
        feats = std::move(sf);
        db = std::move(sdb);
        return true;
    } catch (const Cancelled&) {
        throw;
    } catch (const std::exception& e) {
        st.seconds = now() - t0;
        L::warn(Tag::Map, M::hybrid_failed, {std::string(e.what())});
        return false;
    }
}

bool writeHybridReport(const fs::path& path, const HybridStats& st) {
    std::ofstream f(path);
    if (!f) return false;
    f << "# images image_size features pairs verified putative guided_kept inliers seconds\n";
    f << "hybrid " << st.images << ' ' << st.image_size << ' ' << st.features << ' ' << st.pairs
      << ' ' << st.verified << ' ' << st.putative << ' ' << st.guided_kept << ' ' << st.inliers
      << ' ' << st.seconds << '\n';
    f << "# model images sift restored points_before points_after error_before error_after "
         "points_learned error_combined\n";
    for (size_t k = 0; k < st.models.size(); k++) {
        const HybridModelStats& m = st.models[k];
        f << "model " << k << ' ' << m.images << ' ' << m.sift << ' ' << m.restored << ' '
          << m.points_before << ' ' << m.points_after << ' ' << m.error_before << ' '
          << m.error_after << ' ' << m.points_learned << ' ' << m.error_combined << '\n';
    }
    return (bool)f;
}

}  // namespace sfm
