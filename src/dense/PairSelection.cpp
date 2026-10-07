#include "dense/PairSelection.h"

#include "sfm/feature/Pairing.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>

namespace spirula::dense {
namespace {

bool finite(const sfm::Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

double pose_affinity(const PairImage& a, const PairImage& b) {
    const sfm::Vec3 delta = b.center - a.center;
    const double baseline = delta.norm();
    const sfm::Vec3 direction = delta * (1 / baseline);
    const double angle = std::acos(std::clamp(a.forward.dot(b.forward), -1.0, 1.0));
    const double cone = std::max(0.0, 1 - angle / (a.half_fov_radians + b.half_fov_radians));
    const double facing = std::max(0.0, std::min(a.forward.dot(direction), -b.forward.dot(direction)));
    return (0.01 + std::max(cone, facing)) / baseline;
}

struct RankedPair {
    ImagePair pair;
    uint64_t common = 0;
    double affinity = 0;
};

}  // namespace

void PairOptions::validate() const {
    if (mode != PairMode::Automatic && mode != PairMode::Sequential && mode != PairMode::Exhaustive && mode != PairMode::Explicit)
        throw std::runtime_error("unknown dense pair mode");
    if (neighbors <= 0 || sequence_window <= 0)
        throw std::runtime_error("dense neighbor count and sequence window must be positive");
}

bool shares_points(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b) {
    auto i = a.begin(), j = b.begin();
    while (i != a.end() && j != b.end()) {
        if (*i < *j) ++i;
        else if (*j < *i) ++j;
        else return true;
    }
    return false;
}

std::vector<uint32_t> select_references(const std::vector<PairImage>& images, double fraction, uint64_t seed,
                                      const std::function<void()>& check_cancel) {
    if (!(fraction > 0) || fraction > 1 || images.size() > UINT32_MAX)
        throw std::runtime_error("invalid dense reference selection");
    const size_t count = (size_t)std::ceil(images.size() * fraction);
    std::vector<uint32_t> order(images.size()); std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return images[a].source_image < images[b].source_image; });
    if (count == images.size()) return order;
    sfm::Vec3 mean{};
    for (const auto& image : images) mean = mean + image.center * (1.0 / images.size());
    double scale = 0;
    for (const auto& image : images) scale += (image.center - mean).dot(image.center - mean) / images.size();
    if (!(scale > 0)) scale = 1;
    std::vector<double> nearest(images.size(), std::numeric_limits<double>::infinity());
    std::vector<uint32_t> selected;
    uint32_t next = order[seed % order.size()];
    for (size_t i = 0; i < count; ++i) {
        if (check_cancel) check_cancel();
        selected.push_back(next); nearest[next] = -1;
        const auto& a = images[next]; double farthest = -1;
        for (uint32_t b : order) {
            if (nearest[b] < 0) continue;
            const auto position = a.center - images[b].center, forward = a.forward - images[b].forward, up = a.up - images[b].up;
            nearest[b] = std::min(nearest[b], position.dot(position) / scale + forward.dot(forward) + up.dot(up));
            if (nearest[b] > farthest) { farthest = nearest[b]; next = b; }
        }
    }
    std::sort(selected.begin(), selected.end(), [&](uint32_t a, uint32_t b) { return images[a].source_image < images[b].source_image; });
    return selected;
}

PairStatistics select_pairs(const std::vector<PairImage>& images, const PairOptions& options,
                               const std::function<void(ImagePair)>& emit, const std::function<void()>& check_cancel) {
    options.validate();
    if (!emit || images.size() > UINT32_MAX) throw std::runtime_error("invalid dense pair selection request");
    std::set<int64_t> identities;
    for (const auto& image : images) {
        if (image.source_image < 0 || !identities.insert(image.source_image).second || !finite(image.center) ||
            !finite(image.forward) || std::abs(image.forward.norm() - 1) > 1e-6 ||
            !std::isfinite(image.half_fov_radians) || image.half_fov_radians <= 0 || image.half_fov_radians > 3.141592653589793 ||
            !std::is_sorted(image.visible_points.begin(), image.visible_points.end()) ||
            std::adjacent_find(image.visible_points.begin(), image.visible_points.end()) != image.visible_points.end())
            throw std::runtime_error("dense pair images need unique source identities, finite poses, and sorted distinct visibility tracks");
    }
    PairStatistics statistics;
    auto eligible = [&](ImagePair pair) {
        return pair.first != pair.second && (images[pair.first].center - images[pair.second].center).norm() > 0;
    };
    auto publish = [&](ImagePair pair) {
        if (check_cancel) check_cancel();
        if (options.max_pairs && statistics.emitted >= options.max_pairs) return false;
        if (!eligible(pair)) { ++statistics.rejected_baseline; return true; }
        emit(pair); ++statistics.emitted;
        return true;
    };
    std::vector<uint32_t> order(images.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return images[a].source_image < images[b].source_image;
    });
    if (options.mode == PairMode::Exhaustive) {
        for (size_t a = 0; a < order.size(); ++a)
            for (size_t b = a + 1; b < order.size(); ++b)
                if (!publish(std::minmax(order[a], order[b]))) return statistics;
        return statistics;
    }
    std::vector<ImagePair> pairs;
    if (options.mode == PairMode::Sequential) {
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            return images[a].name != images[b].name ? images[a].name < images[b].name : images[a].source_image < images[b].source_image;
        });
        std::vector<std::string> names;
        for (uint32_t i : order) names.push_back(images[i].name);
        for (const auto& pair : sfm::sequentialPairs((uint32_t)order.size(), options.sequence_window, false, sfm::folderRuns(names)))
            pairs.push_back(std::minmax(order[pair.first], order[pair.second]));
    } else if (options.mode == PairMode::Explicit) {
        pairs = options.explicit_pairs;
        for (auto& pair : pairs) {
            if (pair.first >= images.size() || pair.second >= images.size()) throw std::runtime_error("dense pair list references an unknown image");
            if (pair.first > pair.second) std::swap(pair.first, pair.second);
        }
    } else {
        const size_t k = images.empty() ? 0 : std::min<size_t>(options.neighbors, images.size() - 1);
        auto better = [&](const RankedPair& a, const RankedPair& b) {
            if (a.common != b.common) return a.common > b.common;
            if (a.affinity != b.affinity) return a.affinity > b.affinity;
            const auto aid = std::minmax(images[a.pair.first].source_image, images[a.pair.second].source_image);
            const auto bid = std::minmax(images[b.pair.first].source_image, images[b.pair.second].source_image);
            return aid < bid;
        };
        std::vector<RankedPair> candidates;
        std::vector<std::pair<uint64_t, uint32_t>> observations;
        size_t observation_count = 0;
        for (const auto& image : images) observation_count += image.visible_points.size();
        observations.reserve(observation_count);
        for (uint32_t i = 0; i < images.size(); ++i)
            for (uint64_t point : images[i].visible_points) observations.emplace_back(point, i);
        std::sort(observations.begin(), observations.end());
        std::vector<uint64_t> points;
        std::vector<size_t> offsets;
        for (size_t i = 0; i < observations.size(); ++i) {
            if (i == 0 || observations[i].first != observations[i - 1].first) {
                points.push_back(observations[i].first);
                offsets.push_back(i);
            }
        }
        offsets.push_back(observations.size());
        std::vector<uint64_t> common(images.size());
        std::set<uint32_t> references(options.references.begin(), options.references.end());
        for (uint32_t a : order) {
            if (options.directed && !references.empty() && !references.count(a)) continue;
            if (check_cancel) check_cancel();
            std::fill(common.begin(), common.end(), 0);
            for (uint64_t point : images[a].visible_points) {
                const size_t index = (size_t)(std::lower_bound(points.begin(), points.end(), point) - points.begin());
                for (size_t i = offsets[index]; i < offsets[index + 1]; ++i) ++common[observations[i].second];
            }
            std::vector<RankedPair> best;
            best.reserve(k + 1);
            for (uint32_t b : order) {
                const ImagePair pair = options.directed ? ImagePair{a,b} : ImagePair(std::minmax(a, b));
                if (!eligible(pair)) continue;
                RankedPair item{pair, common[b], pose_affinity(images[a], images[b])};
                auto at = std::lower_bound(best.begin(), best.end(), item, better);
                if (at != best.end() || best.size() < k) best.insert(at, item);
                if (best.size() > k) best.pop_back();
            }
            if (options.directed && !options.max_pairs) {
                for (const auto& item : best) if (!publish(item.pair)) return statistics;
            } else candidates.insert(candidates.end(), best.begin(), best.end());
        }
        std::sort(candidates.begin(), candidates.end(), better);
        candidates.erase(std::unique(candidates.begin(), candidates.end(), [](const RankedPair& a, const RankedPair& b) {
            return a.pair == b.pair;
        }), candidates.end());
        std::vector<int> degrees(images.size(), 0);
        for (const auto& candidate : candidates) {
            const auto pair = candidate.pair;
            if (degrees[pair.first] >= options.neighbors || (!options.directed && degrees[pair.second] >= options.neighbors)) continue;
            if (!publish(pair)) return statistics;
            ++degrees[pair.first]; if (!options.directed) ++degrees[pair.second];
        }
        return statistics;
    }
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    std::sort(pairs.begin(), pairs.end(), [&](ImagePair a, ImagePair b) {
        return std::minmax(images[a.first].source_image, images[a.second].source_image) <
               std::minmax(images[b.first].source_image, images[b.second].source_image);
    });
    for (const auto& pair : pairs) if (!publish(pair)) break;
    return statistics;
}

}  // namespace spirula::dense
