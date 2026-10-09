#include "sfm/Progressive.h"

#include <algorithm>
#include <set>

#include "sfm/core/Cancel.h"
#include "sfm/core/Log.h"
#include "sfm/core/Progress.h"
#include "sfm/feature/Verification.h"
#include "sfm/map/Assemble.h"
#include "i18n/TimeFormat.h"
#include "i18n/catalog/Sfm.h"

namespace sfm {

namespace L = sfm::slog;
namespace M = spirula::i18n::msg::sfm;
using sfm::slog::Tag;
using spirula::i18n::format_duration;

ProgressiveAligner::ProgressiveAligner(const MatchesDatabase& loose,
                                       const std::vector<FeatureSet>& feats,
                                       const SfmConfig& cfg, const VerifyCalibration& calib,
                                       const RigTable& rigs, const SequenceTable& seqs)
    : loose_(loose), feats_(feats), cfg_(cfg), calib_(calib), rigs_(rigs), seqs_(seqs) {}

MatchesDatabase ProgressiveAligner::reverify(double error) const {
    VerificationOptions vo;
    vo.two_view = cfg_.twoview;
    vo.two_view.ransac.max_error = error;
    vo.num_threads = cfg_.threads;
    vo.report = false;
    const CameraSetup& cs = calib_.cameras;
    BearingCache bc;
    if (cs.anyWide()) {
        bc = precomputeBearings(feats_, perImageCameras(cs, feats_.size()), !cs.mixed(),
                                cfg_.threads);
        vo.bearings = &bc;
    }
    std::vector<Camera> percam;
    if (calib_.priors && cfg_.sensor_verify && calib_.priors->anyRotation()) {
        percam = perImageCameras(cs, feats_.size());
        vo.priors = calib_.priors.get();
        vo.cameras = &percam;
    }
    std::vector<std::pair<uint32_t, uint32_t>> pairs;
    pairs.reserve(loose_.pairs.size());
    for (const TwoViewMatches& t : loose_.pairs) pairs.emplace_back(t.image1, t.image2);
    auto stored = [&](size_t b, size_t e, std::vector<std::vector<FeatureMatch>>& out) {
        out.resize(e - b);
        for (size_t p = b; p < e; p++) out[p - b] = loose_.pairs[p].matches.toVector();
    };

    MatchesDatabase db;
    db.images = loose_.images;
    db.cameras = loose_.cameras;
    db.camera_ids = loose_.camera_ids;
    db.focal_prior = loose_.focal_prior;
    db.focal_measured = loose_.focal_measured;
    db.pairs = verifyPairs(feats_, pairs, stored, vo);
    return db;
}

void ProgressiveAligner::startAttempt(double error, const MatchesDatabase& db) {
    cfg_.mapper.max_reproj_error = error;
    cfg_.manager.merge.filter_reproj_error = error;
    mapper_ = std::make_unique<Mapper>(db, feats_, cfg_.mapper, calib_.cameras.ids, &rigs_,
                                       &seqs_,
                                       cfg_.sensor_map ? calib_.positionPriors() : nullptr);
}

std::vector<Reconstruction> ProgressiveAligner::run(AssembleStats& ast) {
    const std::vector<double> errors = cfg_.progressiveErrors();
    const bool verbose = !cfg_.quiet;
    std::vector<Reconstruction> models;
    for (size_t k = 0; k < errors.size(); k++) {
        cancel::check();
        const double t0 = now();
        const double e = errors[k];
        ProgressiveAttempt a;
        a.error = e;
        // The mapper holds a reference to its database, so the old one goes
        // only after the new mapper replaces the mapper built on it.
        std::unique_ptr<MatchesDatabase> prev = std::move(db_);
        if (k == 0) {
            startAttempt(e, loose_);
            a.pairs = loose_.pairs.size();
        } else {
            db_ = std::make_unique<MatchesDatabase>(reverify(e));
            startAttempt(e, *db_);
            a.pairs = db_->pairs.size();
        }
        prev.reset();
        if (verbose)
            L::out(Tag::Map, M::progressive_attempt,
                   {(long long)(k + 1), (long long)errors.size(), L::num(e, 1),
                    (long long)a.pairs});
        ast = AssembleStats();
        if (k == 0) {
            models = runMapper(*mapper_, loose_, feats_, cfg_, ast);
        } else {
            // What the tighter gate no longer supports is dropped by the
            // refinement itself; growth then retries every image from scratch.
            for (Reconstruction& m : models)
                if (m.numRegistered() >= 2) m = mapper_->refine(m);
            AssembleOptions ao = cfg_.assemble;
            ao.verbose = verbose;
            ao.tag = "map";
            models = assembleModels(*mapper_, std::move(models), cfg_.manager, ao, ast);
        }
        std::set<uint32_t> any;
        for (const Reconstruction& m : models) {
            a.largest = std::max(a.largest, m.numRegistered());
            for (const auto& kv : m.images)
                if (kv.second.registered) any.insert(kv.first);
        }
        a.registered = (uint32_t)any.size();
        a.models = models.size();
        a.seconds = now() - t0;
        attempts_.push_back(a);
        if (verbose)
            L::out(Tag::Map, M::progressive_attempt_done,
                   {(long long)(k + 1), (long long)errors.size(), (long long)a.largest,
                    (long long)a.registered, (long long)a.models,
                    format_duration(a.seconds)});
    }
    if (!models.empty()) sfm::progress::model(models.front(), /*force=*/true);
    return models;
}

}  // namespace sfm
