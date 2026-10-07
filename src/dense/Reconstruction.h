#pragma once

#include "dense/DenseConfig.h"
#include "data/DatasetParser.h"

#include <atomic>
#include <array>
#include <memory>

namespace spirula::dense {

struct ViewPixels {
    int width = 0, height = 0;
    std::vector<float> rgb;
    std::vector<uint8_t> keep;
};

struct ReconstructionStatistics {
    uint64_t tested = 0, masked = 0, low_overlap = 0, cycle = 0, geometry = 0;
    uint64_t triangulated = 0, insufficient_support = 0, refined = 0, fused = 0, exported = 0;
    double resolved_voxel_size = 0;
    uint64_t refinement_workers = 1;
    uint64_t fusion_workers = 1, fusion_partitions = 0, fusion_boundary_candidates = 0;
    double reference_work_seconds = 0;
    double max_reference_reprojection_error = 0, sum_reference_max_reprojection_error = 0;
    uint64_t min_reference_support = 0, max_reference_support = 0, sum_reference_support = 0;
    std::array<uint64_t,16> reference_reprojection_histogram{};
};

using DenseProgress = std::function<void(const char*, uint64_t, uint64_t)>;

struct PreviewCheckpoint {
    std::string file, error;
    uint64_t points = 0;
    bool provisional = true;
    bool filtered = false;
};

class Reconstruction {
public:
    Reconstruction(const std::string& work_dir, const std::vector<View>& views,
                   const DenseConfig& config, const std::atomic<bool>* cancel = nullptr,
                   const std::string& preview_dir = {});
    ~Reconstruction();
    void add_pair(uint32_t a, uint32_t b, const ViewPixels& pixels_a, const ViewPixels& pixels_b,
                  const roma::PairPrediction& prediction);
    void complete_reference(uint32_t reference, bool background = false);
    // The published prefix is immutable; final points use a separate file.
    PreviewCheckpoint checkpoint();
    // Writes only a temporary output; the application publishes after a nonempty success.
    ReconstructionStatistics finish(const std::string& temporary_ply, const ParsedDataset& dataset,
                                     const DenseProgress& progress = {});
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace spirula::dense
