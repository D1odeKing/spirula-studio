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

// One matchable view. Faces split from one image share source_image and are never paired.
struct PairImage {
    int64_t source_image = -1;
    int face = 0;
    std::string name;
    sfm::Vec3 center, forward{0, 0, 1};
    sfm::Vec3 up{0, 1, 0};
    double half_fov_radians = 0.8;
    std::vector<uint64_t> visible_points;
    // Paired only through shared sparse points, never by pose alone.
    bool shared_points_only = false;
};

struct PairOptions {
    PairMode mode = PairMode::Automatic;
    // Undirected automatic mode: each part of a reference's view is matched against about this
    // many neighbours, up to twice as many in all. Directed mode: neighbours per reference.
    int neighbors = 8, sequence_window = 8;
    // Undirected automatic mode: references are added until every surface cell is seen by this
    // many of them; 0 makes every view one.
    int reference_coverage = 0;
    uint64_t max_pairs = 0;
    std::vector<ImagePair> explicit_pairs;
    bool directed = false;
    std::vector<uint32_t> references;
    // Automatic mode only: a pair farther apart is refused and the next-ranked image takes its place.
    double max_baseline = 0;
    void validate() const;
};

struct PairStatistics {
    uint64_t emitted = 0, rejected_baseline = 0, references = 0, cells = 0;
};

// How often planned pairs see each surface cell. A view reconstructs a cell when two or more of
// its partners see it too; medians and means are over cells that three or more views see.
struct SurfaceCoverage {
    uint64_t cells = 0, reconstructable = 0, covered = 0;
    double references = 0, mean_references = 0, neighbors = 0, mean_neighbors = 0;
};

bool shares_points(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b);
std::vector<uint32_t> select_references(const std::vector<PairImage>& images, double fraction, uint64_t seed,
                                      const std::function<void()>& check_cancel = {});

// Exhaustive emits incrementally. Automatic pairs views by the sparse points they share and,
// given the points' positions, prefers neighbours that see them from a wider angle.
PairStatistics select_pairs(const std::vector<PairImage>& images, const PairOptions& options,
                               const std::function<void(ImagePair)>& emit,
                               const std::function<void()>& check_cancel = {},
                               const std::vector<sfm::Vec3>& points = {});

SurfaceCoverage measure_coverage(const std::vector<PairImage>& images, const std::vector<ImagePair>& pairs,
                                 const std::vector<sfm::Vec3>& points, bool directed);

// The automatic-mode baseline limit: `setting` when positive, 3x the median planned
// baseline when 0 on a metric model, else none (0). `refused` counts planned pairs over it.
double resolve_max_baseline(const std::vector<PairImage>& images, const PairOptions& options, double setting,
                            bool metric, uint64_t* refused = nullptr, const std::function<void()>& check_cancel = {},
                            const std::vector<sfm::Vec3>& points = {});

}  // namespace spirula::dense
