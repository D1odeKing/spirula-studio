#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>

namespace spirula::dense {

struct Surface {
    double point[3]{}, normal[3]{}, radius = 0;
    float color[3]{};
    uint32_t support = 0;
    int64_t cell[3]{};
};

struct FusionStatistics {
    uint64_t surfaces = 0, partitions = 0, boundary_candidates = 0;
    unsigned workers = 1;
};

// `progress(permille, 1000)` covers every step, sorts included, weighted by cost.
FusionStatistics fuse_surfaces(const std::filesystem::path& source, const std::filesystem::path& destination,
    const std::filesystem::path& work, double voxel, uint64_t budget, unsigned requested_workers,
    const std::function<void()>& check = {}, const std::function<void(uint64_t,uint64_t)>& progress = {});

}  // namespace spirula::dense
