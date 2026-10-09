#pragma once

// Progressive alignment (docs/notes/sfm-progressive-alignment.md): the mapper
// runs once per pixel error, loose to tight, every attempt continuing from the
// last. The matches are verified once, at the loosest error; each tighter
// attempt re-verifies those inliers at its own, so no pair the mapper sees was
// verified looser than its own gate (D47).

#include "sfm/Pipeline.h"

#include <memory>
#include <vector>

namespace sfm {

struct ProgressiveAttempt {
    double error = 0;          // extraction pixels
    size_t pairs = 0;          // verified pairs the attempt mapped with
    uint32_t largest = 0;      // images in the largest model after it
    uint32_t registered = 0;   // distinct images in any model
    size_t models = 0;
    double seconds = 0;
};

class ProgressiveAligner {
public:
    // `loose` was verified at cfg.progressive_error_start; it, `feats`, `calib`,
    // `rigs` and `seqs` must outlive the aligner.
    ProgressiveAligner(const MatchesDatabase& loose, const std::vector<FeatureSet>& feats,
                       const SfmConfig& cfg, const VerifyCalibration& calib,
                       const RigTable& rigs, const SequenceTable& seqs);

    std::vector<Reconstruction> run(AssembleStats& ast);

    // The last attempt's, at the end error: what the finishing passes run with.
    Mapper& mapper() { return *mapper_; }
    const std::vector<ProgressiveAttempt>& attempts() const { return attempts_; }

private:
    MatchesDatabase reverify(double error) const;
    void startAttempt(double error, const MatchesDatabase& db);

    const MatchesDatabase& loose_;
    const std::vector<FeatureSet>& feats_;
    SfmConfig cfg_;
    const VerifyCalibration& calib_;
    const RigTable& rigs_;
    const SequenceTable& seqs_;
    std::unique_ptr<MatchesDatabase> db_;   // the current attempt's, past the first
    std::unique_ptr<Mapper> mapper_;
    std::vector<ProgressiveAttempt> attempts_;
};

}  // namespace sfm
