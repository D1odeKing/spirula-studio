#pragma once

// The part of a video a dataset uses, as the source frames it covers. One
// rounding for every reader of a clip: both decoders, the progress estimate
// and the preview panels all have to name the same first and last frame.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace app {

// Source frames [first, end); `end` < 0 runs to the end of the stream.
// Frame i is shown at i / fps.
struct FrameSpan {
    int64_t first = 0, end = -1;
    bool past(int64_t i) const { return end >= 0 && i >= end; }
    int64_t length(int64_t frames) const {
        const int64_t e = end >= 0 && (frames <= 0 || end < frames) ? end : frames;
        return std::max<int64_t>(0, e - first);
    }
};

// Seconds of the video; `clip_end` 0 is its end.
inline FrameSpan clip_frames(double clip_start, double clip_end, double fps) {
    FrameSpan s;
    if (!(fps > 0.0)) return s;
    if (clip_start > 0.0) s.first = (int64_t)std::ceil(clip_start * fps - 1e-6);
    if (clip_end > 0.0)
        s.end = std::max(s.first + 1, (int64_t)std::ceil(clip_end * fps - 1e-6));
    return s;
}

}  // namespace app
