#pragma once

#include "data/CameraMath.h"
#include "sfm/geometry/Triangulation.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace spirula::dense {

template<class T> class DiskArray;

struct View {
    int64_t source_image = -1;
    camhost::Camera camera;
    sfm::Mat3 world_to_camera{1,0,0, 0,1,0, 0,0,1};
    sfm::Vec3 center;
    bool source_camera = false;
    // Matcher cells per pixel along x and y; tolerances are measured in these units.
    double grid_scale[2] = {1, 1};
    void validate() const;
};

struct GeometryOptions {
    double min_angle_degrees = 1;
    double max_reprojection_error = 2;
    double max_relative_depth_error = 0.01;
    int min_source_images = 3;
    void validate() const;
};

struct Observation {
    const View* view = nullptr;
    sfm::Vec2 pixel;
    double overlap = 1;
    double precision_xx = 1, precision_xy = 0, precision_yy = 1;
};

struct PointEstimate {
    sfm::Vec3 position;
    std::vector<uint32_t> observations;
    double max_reprojection_error = 0;
};

bool bearing(const View& view, const sfm::Vec2& pixel, sfm::Vec3& ray);
bool valid_observation(const Observation& observation);
bool project(const View& view, const sfm::Vec3& point, sfm::Vec2& pixel);
double depth(const View& view, const sfm::Vec3& point, const sfm::Vec3* reference_ray = nullptr);
sfm::Vec2 pixel_residual(const View& view, const sfm::Vec2& a, const sfm::Vec2& b);
double residual_length(const View& view, const sfm::Vec2& residual);
// Infinite behind the observed ray or where the camera cannot see the point.
double reprojection_error(const Observation& observation, const sfm::Vec3& point);
bool triangulate(const Observation& a, const Observation& b,
                   const GeometryOptions& options, sfm::Vec3& point);
// Refines only the point; at most one face from each original image contributes.
bool refine_point(const sfm::Vec3& initial, const std::vector<Observation>& observations,
                    const GeometryOptions& options, PointEstimate& result);
// Observations must have distinct source IDs in ascending order.
bool refine_unique_point(const sfm::Vec3& initial, const DiskArray<Observation>& observations,
                         const GeometryOptions& options, DiskArray<uint32_t>& retained, PointEstimate& result,
                         const std::function<void()>& check = {});
bool depth_consistent(const View& reference, const sfm::Vec3& a, const sfm::Vec3& b,
                        double max_relative_error, const sfm::Vec3* reference_ray = nullptr);

}  // namespace spirula::dense
