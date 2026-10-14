#pragma once

// Where a cloud is and how big it is, for framing it in a viewport: shared by
// the model viewer (SplatViewer.cpp) and the dataset screen's input preview.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gui {

// The median centre, and twice the median distance from it as the radius: a
// trained scene's floaters put a bounding box a kilometre away. The web viewer
// frames a model the same way (viewer/src/viewer.cpp, ssv_fit_sphere).
template <typename T>
inline void scene_extent(const std::vector<T>& xyz, int64_t n,
                         float center[3], float& radius) {
    center[0] = center[1] = center[2] = 0.0f;
    radius = 1.0f;
    if (n <= 0) return;
    // A median is no better for being taken over more of the same cloud.
    const int64_t step = std::max<int64_t>(1, n / (1 << 20));
    std::vector<float> tmp;
    tmp.reserve((size_t)(n / step + 1));
    for (int d = 0; d < 3; d++) {
        tmp.clear();
        for (int64_t i = 0; i < n; i += step) tmp.push_back((float)xyz[(size_t)i * 3 + d]);
        std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
        center[d] = tmp[tmp.size() / 2];
    }
    tmp.clear();
    for (int64_t i = 0; i < n; i += step) {
        const float dx = (float)xyz[(size_t)i * 3 + 0] - center[0];
        const float dy = (float)xyz[(size_t)i * 3 + 1] - center[1];
        const float dz = (float)xyz[(size_t)i * 3 + 2] - center[2];
        tmp.push_back(dx * dx + dy * dy + dz * dz);
    }
    std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
    radius = 2.0f * std::sqrt(std::max(tmp[tmp.size() / 2], 1e-24f));
    radius = std::max(radius, 1e-6f);
}

// One client unit in model units: where the web viewer's fitSphere puts the
// camera for that radius at this viewport's 90-degree field of view.
inline float view_distance_for(float radius) { return 1.25f * radius; }

}  // namespace gui
