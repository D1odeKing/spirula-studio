#pragma once

// The hybrid frontend (docs/notes/sfm-hybrid-frontend-plan.md): a learned
// frontend's cameras, then SIFT matched under their geometry, triangulated
// against them and bundle-adjusted. Only poses cross over, never feature rows.

#include "sfm/Pipeline.h"

#include <filesystem>
#include <string>
#include <vector>

namespace sfm {

struct HybridModelStats {
    uint32_t images = 0;     // registered by the learned frontend
    uint32_t sift = 0;       // still registered after the SIFT solve
    uint32_t restored = 0;   // put back from the learned pose, without points
    size_t points_before = 0, points_after = 0;
    double error_before = 0, error_after = 0;   // mean, source pixels
    // With SS_SFM_HYBRID_LEARNED_POINTS: the learned tracks added, and the
    // mean error over both families.
    size_t points_learned = 0;
    double error_combined = 0;
};

struct HybridStats {
    size_t images = 0;       // SIFT extracted on
    int image_size = 0, features = 0;
    size_t pairs = 0, verified = 0;
    uint64_t putative = 0, guided_kept = 0;   // matches before / after the epipolar gate
    uint64_t inliers = 0;
    double seconds = 0;
    std::vector<HybridModelStats> models;
};

// Replaces `models`, `feats` and `db` with their SIFT counterparts and returns
// true; on any failure but a cancel, leaves all three untouched and returns
// false. The caller's mapper must already be released: it refers to `db`.
bool hybridSift(std::vector<Reconstruction>& models, std::vector<FeatureSet>& feats,
                MatchesDatabase& db, const SfmConfig& cfg, const VerifyCalibration& calib,
                const RigTable& rigs, const SequenceTable& seqs, const std::string& image_dir,
                const std::filesystem::path& workspace, HybridStats& st);

// One line per model, read by programs, so untranslated.
bool writeHybridReport(const std::filesystem::path& path, const HybridStats& st);

}  // namespace sfm
