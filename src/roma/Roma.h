#pragma once

#include <cstdint>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace spirula::roma {

enum class InferencePrecision { Automatic, Float32, Mixed };

struct MatchOptions {
    int low_width = 800, low_height = 800;
    int high_width = 1280, high_height = 1280;
    bool bidirectional = true;
    float overlap_saturation = -1;
    uint64_t memory_budget_bytes = 0;
    InferencePrecision precision = InferencePrecision::Automatic;
    std::function<void(const char*)> progress;
    static MatchOptions preset(const std::string& name) {
        MatchOptions out;
        if (name == "precise") return out;
        if (name == "turbo") out.low_width = out.low_height = 320;
        else if (name == "fast") out.low_width = out.low_height = 512;
        else if (name == "base") out.low_width = out.low_height = 640;
        else throw std::runtime_error("unknown RoMa preset: " + name);
        out.high_width = out.high_height = 0;
        out.bidirectional = false;
        return out;
    }
    void validate() const {
        if (precision != InferencePrecision::Automatic && precision != InferencePrecision::Float32 && precision != InferencePrecision::Mixed)
            throw std::runtime_error("unknown RoMa inference precision");
        if (!std::isfinite(overlap_saturation) || !(overlap_saturation == -1 || (overlap_saturation >= 0 && overlap_saturation <= 1)))
            throw std::runtime_error("RoMa overlap saturation must be disabled (-1) or in [0,1]");
        auto valid_size = [](int w, int h, int granularity) {
            // The largest fine feature map has 64 channels and signed 32-bit shader indices.
            return w > 0 && h > 0 && (int64_t)w * h <= std::numeric_limits<int32_t>::max() / 64 &&
                w % granularity == 0 && h % granularity == 0;
        };
        if (!valid_size(low_width, low_height, 16))
            throw std::runtime_error("RoMa low-resolution dimensions must be positive multiples of 16 within the supported pixel limit");
        if (!((high_width == 0 && high_height == 0) || valid_size(high_width, high_height, 4)))
            throw std::runtime_error("RoMa high-resolution dimensions must both be zero or positive multiples of 4 within the supported pixel limit");
    }
};

struct Prediction {
    int width = 0, height = 0;
    // Target (x,y) coordinates in [-1,1], with align_corners=false.
    std::vector<float> warp;
    std::vector<float> overlap;
    // Pixel precision in symmetric (xx,xy,yy) order at the matching resolution.
    std::vector<float> precision;
};

struct PairPrediction {
    Prediction forward, backward;
};

struct FeatureCacheStatistics {
    uint64_t hits = 0, misses = 0, evictions = 0;
    uint64_t bytes = 0, peak_bytes = 0, limit_bytes = 0;
};

// One inference thread per session; all sessions share the nn Vulkan device.
class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    void load(const std::string& checkpoint, InferencePrecision precision = InferencePrecision::Float32,
              const std::function<void(uint64_t, uint64_t)>& progress = {});
    static InferencePrecision resolvePrecision(InferencePrecision precision);
    InferencePrecision precision() const;
    void unload();
    bool loaded() const;
    // RGB inputs are interleaved floats in [0,1]; each image can have its own size.
    PairPrediction match(const float* a, int width_a, int height_a,
                           const float* b, int width_b, int height_b,
                           const MatchOptions& options = {});
    // IDs identify distinct immutable RGB views until the cache is cleared or weights are reloaded.
    PairPrediction matchCached(uint64_t image_a, const float* a, int width_a, int height_a,
                                 uint64_t image_b, const float* b, int width_b, int height_b,
                                 const MatchOptions& options = {});
    // An empty budget follows device headroom; zero disables caching.
    void setFeatureCacheBudget(std::optional<uint64_t> bytes = {});
    void clearFeatureCache();
    FeatureCacheStatistics featureCacheStatistics() const;
    uint64_t deviceBytes() const;
    uint64_t peakScratchBytes() const;
    static uint64_t plannedScratchBytes(const MatchOptions& options);

private:
    PairPrediction matchImpl(const float* a, int width_a, int height_a,
                               const float* b, int width_b, int height_b,
                               const MatchOptions& options, const uint64_t* image_ids);
    struct Impl;
    Impl* impl_;
};

}  // namespace spirula::roma
