#pragma once

#include "sfm/geometry/LinAlg.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace spirula::dense {

using ImagePair = std::pair<uint32_t, uint32_t>;
enum class PairMode { Automatic, Sequential, Exhaustive, Explicit };

struct PairImage {
    int64_t source_image = -1;
    std::string name;
    sfm::Vec3 center, forward{0, 0, 1};
    sfm::Vec3 up{0, 1, 0};
    double half_fov_radians = 0.8;
    std::vector<uint64_t> visible_points;
};

struct PairOptions {
    PairMode mode = PairMode::Automatic;
    int neighbors = 8, sequence_window = 8;
    uint64_t max_pairs = 0;
    std::vector<ImagePair> explicit_pairs;
    bool directed = false;
    std::vector<uint32_t> references;
    void validate() const;
};

struct PairStatistics {
    uint64_t emitted = 0, rejected_baseline = 0;
};

bool shares_points(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b);
std::vector<uint32_t> select_references(const std::vector<PairImage>& images, double fraction, uint64_t seed,
                                      const std::function<void()>& check_cancel = {});

// Exhaustive emits incrementally; automatic stores at most N*neighbors candidates.
PairStatistics select_pairs(const std::vector<PairImage>& images, const PairOptions& options,
                               const std::function<void(ImagePair)>& emit,
                               const std::function<void()>& check_cancel = {});

}  // namespace spirula::dense
