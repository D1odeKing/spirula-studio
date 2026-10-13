#pragma once
// Which sparse points each view sees, for pairing a dataset that has no SfM tracks
// (Nerfstudio, Metashape, a laser scan) or whose tracks do not match its points.

#include "dense/Geometry.h"

#include <functional>
#include <vector>

namespace spirula::dense {

struct ProjectedPoints {
    std::vector<sfm::Vec3> points;
    std::vector<std::vector<uint64_t>> visible;   // per view, sorted indices into points
};

// A sample of the points, one per small cell of the surface, projected into every view; one 15%
// behind the nearest in its cell of a coarse grid over the frame is hidden. Without points, each
// view's rays stand in for its surface, at the depth where its optical axis meets its neighbours'.
ProjectedPoints project_visibility(const std::vector<View>& views, const std::vector<sfm::Vec3>& points,
                                   const std::function<void()>& check_cancel = {});

}  // namespace spirula::dense
