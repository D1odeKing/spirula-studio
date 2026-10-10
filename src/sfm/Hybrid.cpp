#include "sfm/Hybrid.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
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

// Guided matching: a putative match is kept when it lies within `tol` of the
// epipolar plane the model's two poses give it, measured as an angle on each
// image's own bearings so an equirectangular image is gated like a pinhole one.
class EpipolarGate {
public:
    EpipolarGate(const std::vector<Reconstruction>& models, const std::vector<FeatureSet>& feats,
                 double tol_px)
        : models_(models), feats_(feats), tol_px_(tol_px) {}

    void filter(int k, uint32_t i, uint32_t j, std::vector<FeatureMatch>& m) {
        const Reconstruction& rec = models_[k];
        const Image& a = rec.images.at(i);
        const Image& b = rec.images.at(j);
        const Camera& ca = rec.cameras.at(a.camera_id);
        const Camera& cb = rec.cameras.at(b.camera_id);
        const std::vector<Vec3>& ba = bearings(k, i, ca);
        const std::vector<Vec3>& bb = bearings(k, j, cb);
        const Mat3 R = mul(b.pose.R, transpose(a.pose.R));
        const Vec3 t = b.pose.t - mul(R, a.pose.t);
        const double ta = tol_px_ * feats_[i].pixelScale() / std::max(1e-6, ca.focal());
        const double tb = tol_px_ * feats_[j].pixelScale() / std::max(1e-6, cb.focal());
        const double tol = std::sin(std::min(0.5, std::max(ta, tb)));
        const double tn = t.norm();
        const double cam_dist = std::max(cameraCenter(a.pose).norm(), cameraCenter(b.pose).norm());
        const bool rotation_only = !(tn > 1e-9 * std::max(1.0, cam_dist));
        size_t w = 0;
        for (const FeatureMatch& fm : m) {
            const Vec3 ra = mul(R, ba[fm.idx1]);
            const Vec3& rb = bb[fm.idx2];
            double e;
            if (rotation_only) {
                e = (ra - rb).norm();
            } else {
                const Vec3 nrm = t.cross(ra);
                const double nn = nrm.norm();
                e = nn > 1e-12 ? std::fabs(nrm.dot(rb)) / nn : 0.0;
            }
            if (e <= tol) m[w++] = fm;
        }
        m.resize(w);
    }

private:
    const std::vector<Vec3>& bearings(int k, uint32_t img, const Camera& cam) {
        auto key = std::make_pair(k, img);
        auto it = cache_.find(key);
        if (it != cache_.end()) return it->second;
        std::vector<Vec3> v(feats_[img].count());
        for (uint32_t f = 0; f < feats_[img].count(); f++) {
            const Keypoint& kp = feats_[img].keypoints[f];
            v[f] = cam.bearing({kp.x, kp.y}).normalized();
        }
        return cache_.emplace(key, std::move(v)).first->second;
    }

    const std::vector<Reconstruction>& models_;
    const std::vector<FeatureSet>& feats_;
    double tol_px_;
    std::map<std::pair<int, uint32_t>, std::vector<Vec3>> cache_;
};

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
        const bool guided = !spirula::env("SFM_HYBRID_GUIDED") || spirula::env_on("SFM_HYBRID_GUIDED");
        sc.match = MatchOptions();
        sc.match.device = cfg.match.device;
        sc.match.device_selector = cfg.match.device_selector;
        sc.match.batch_pairs = cfg.match.batch_pairs;
        sc.match.descriptor_budget_bytes = cfg.match.descriptor_budget_bytes;
        sc.match.max_num_matches = cfg.match.max_num_matches;
        // Under the epipolar gate the ratio test only has to reject the
        // ambiguous, not the wrong: repeated texture is what the gate is for.
        sc.match.max_ratio = (float)envNumber("SFM_HYBRID_RATIO", guided ? 0.9 : 0.8);
        sc.match.cross_check = envNumber("SFM_HYBRID_CROSS_CHECK", 1) != 0;
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
        std::unique_ptr<IFeatureMatcher> matcher =
            createFeatureMatcher(sc.matcher, sc.match, sc.lightglue, sc.loma_match);
        EpipolarGate gate(models, sf, envNumber("SFM_HYBRID_EPIPOLAR_PX", 1.5 * cfg.max_error));
        uint64_t putative = 0, kept = 0;
        auto matchFn = [&](size_t b, size_t e, std::vector<std::vector<FeatureMatch>>& out) {
            matcher->matchBatch(sf, pairs, b, e, out);
            for (size_t p = b; p < e; p++) {
                std::vector<FeatureMatch>& m = out[p - b];
                putative += m.size();
                if (guided) gate.filter(owner[p], pairs[p].first, pairs[p].second, m);
                kept += m.size();
            }
        };
        VerificationOptions vo;
        vo.two_view = cfg.twoview;
        vo.two_view.ransac.max_error = cfg.max_error;
        vo.num_threads = cfg.threads;
        vo.match_batch_pairs = sc.match.batch_pairs;
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
            L::diag(Tag::Match, "[hybrid] epipolar gate kept %llu of %llu putative matches",
                    (unsigned long long)kept, (unsigned long long)putative);

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
