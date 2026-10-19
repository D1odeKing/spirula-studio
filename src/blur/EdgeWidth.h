#pragma once

// The edge-width blur measure's constants and its two histogram reductions,
// shared by the host walk (app/FrameSharpness.cpp, edge_width_px) and the GPU
// one (blur/EdgeWidthGpu.h, shaders/edge_width.slang). Host-only, no device:
// a build without the inference layer includes it too.

#include <cstdint>

namespace blur {

// |Sobel| of 8-bit luma stays under 1024 levels; a bin is a quarter level.
inline constexpr int   kMagBins = 4096;
inline constexpr float kMagPerLevel = 4.0f;
// How far an edge is walked each way, in half-size pixels, and the rise it
// must make to count: a real step, in 8-bit levels, not a ripple.
inline constexpr int   kReach = 60;
inline constexpr float kStep = 12.0f;
inline constexpr int   kWidthBins = 2 * kReach + 1;

inline int mag_bin(float a) {
    const int b = (int)(a * kMagPerLevel);
    return b < kMagBins - 1 ? b : kMagBins - 1;
}

// The lower edge of the bin that holds the 99th percentile of `n` samples: an
// edge is a gradient above it. Not the upper: a synthetic ramp puts all its
// edges in one bin, and that would leave none.
inline float strong_from_hist(const uint32_t* hist, uint64_t n) {
    const uint64_t at = n * 99 / 100;
    uint64_t c = 0;
    for (int b = 0; b < kMagBins; b++) {
        c += hist[b];
        if (c > at) return (float)b / kMagPerLevel;
    }
    return (float)(kMagBins - 1) / kMagPerLevel;
}

// The mean of the sharpest fifth of the widths, less the narrowest twentieth:
// blur widens every edge, while a sharp frame of softly shaded things has wide
// edges among narrow ones. Negative for fewer than 30 edges.
inline float width_from_hist(const uint32_t* hist) {
    uint64_t n = 0;
    for (int b = 0; b < kWidthBins; b++) n += hist[b];
    if (n < 30) return -1.0f;
    const uint64_t lo = n / 20, hi = n / 4;
    uint64_t rank = 0;
    double sum = 0.0;
    for (int b = 0; b < kWidthBins && rank < hi; b++) {
        const uint64_t from = rank > lo ? rank : lo, to = rank + hist[b] < hi ? rank + hist[b] : hi;
        if (to > from) sum += (double)b * (double)(to - from);
        rank += hist[b];
    }
    return (float)(sum / (double)(hi - lo));
}

}  // namespace blur
