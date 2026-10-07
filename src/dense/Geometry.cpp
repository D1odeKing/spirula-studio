#include "dense/Geometry.h"

#include "core/CameraModel.h"
#include "dense/DiskArray.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace spirula::dense {
namespace {

bool finite(const sfm::Vec3& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }

sfm::Vec3 camera_point(const View& view, const sfm::Vec3& point) {
    return sfm::mul(view.world_to_camera, point - view.center);
}

sfm::Mat34 projection(const View& view, const sfm::Vec3& origin) {
    const sfm::Vec3 t = sfm::mul(view.world_to_camera, origin - view.center);
    const auto& r = view.world_to_camera;
    return {r[0],r[1],r[2],t.x, r[3],r[4],r[5],t.y, r[6],r[7],r[8],t.z};
}

bool valid(const Observation& o) {
    double px[2] = {o.pixel.x,o.pixel.y};
    return o.view && o.view->source_image >= 0 && std::isfinite(o.pixel.x) && std::isfinite(o.pixel.y) &&
        camhost::normalize_pixel(o.view->camera,px) &&
        std::isfinite(o.overlap) && o.overlap > 0 && o.overlap <= 1 &&
        std::isfinite(o.precision_xx) && std::isfinite(o.precision_xy) && std::isfinite(o.precision_yy) &&
        o.precision_xx >= 0 && o.precision_yy >= 0 && o.precision_xx + o.precision_yy > 0 &&
        o.precision_xx * o.precision_yy >= o.precision_xy * o.precision_xy -
            1e-6 * std::max(o.precision_xx * o.precision_yy, o.precision_xy * o.precision_xy);
}

double error(const Observation& o, const sfm::Vec3& point) {
    sfm::Vec3 ray;
    if (!bearing(*o.view,o.pixel,ray) || camera_point(*o.view,point).dot(ray) <= 0) return std::numeric_limits<double>::infinity();
    sfm::Vec2 pixel;
    if (!project(*o.view, point, pixel)) return std::numeric_limits<double>::infinity();
    return residual_length(*o.view, pixel_residual(*o.view, pixel, o.pixel));
}

template<class Observations, class Indices>
double cost(const Observations& observations, const Indices& indices,
              const sfm::Vec3& point, double huber, const std::function<void()>& check) {
    double total = 0;
    for (uint32_t i : indices) {
        if (check) check();
        const auto& o = observations[i];
        sfm::Vec2 px;
        if (!project(*o.view, point, px)) return std::numeric_limits<double>::infinity();
        const auto delta = pixel_residual(*o.view, px, o.pixel);
        const double dx = delta.x, dy = delta.y;
        const double d = std::sqrt(std::max(0.0, o.precision_xx * dx * dx + 2 * o.precision_xy * dx * dy + o.precision_yy * dy * dy));
        total += o.overlap * (d <= huber ? 0.5 * d * d : huber * (d - 0.5 * huber));
    }
    return total;
}

template<class Observations, class Indices>
bool optimize(const Observations& observations, const Indices& indices,
                 double huber, sfm::Vec3& point, const std::function<void()>& check) {
    for (int iteration = 0; iteration < 12; ++iteration) {
        sfm::Mat3 normal{};
        double gradient[3] = {};
        for (uint32_t index : indices) {
            if (check) check();
            const auto& o = observations[index];
            const auto& view = *o.view;
            const sfm::Vec3 q = camera_point(view, point);
            sfm::Vec2 px;
            if (!project(view, point, px)) return false;
            const auto residual = pixel_residual(view, px, o.pixel);
            const double dx = residual.x, dy = residual.y;
            const double norm = std::sqrt(std::max(0.0, o.precision_xx * dx * dx + 2 * o.precision_xy * dx * dy + o.precision_yy * dy * dy));
            const double weight = o.overlap * (norm > huber ? huber / norm : 1);
            double jx[3], jy[3];
            const auto& r = view.world_to_camera;
            if (view.source_camera && (view.camera.model != (int)CameraModelType::PINHOLE ||
                                       view.camera.tier != (int)CameraDistortionType::None || view.camera.source_model >= 0)) {
                const double ray[3] = {q.x, q.y, q.z}; double j[6];
                if (!camhost::projection_jacobian(view.camera, ray, j)) return false;
                for (int c = 0; c < 3; ++c) {
                    jx[c] = jy[c] = 0;
                    for (int k = 0; k < 3; ++k) { jx[c] += j[k] * r[k * 3 + c]; jy[c] += j[3 + k] * r[k * 3 + c]; }
                }
            } else {
                for (int c = 0; c < 3; ++c) {
                    jx[c] = view.camera.fx / q.z * (r[c] - q.x / q.z * r[6 + c]);
                    jy[c] = view.camera.fy / q.z * (r[3 + c] - q.y / q.z * r[6 + c]);
                }
            }
            for (int a = 0; a < 3; ++a) {
                gradient[a] += weight * (jx[a] * (o.precision_xx * dx + o.precision_xy * dy) +
                                         jy[a] * (o.precision_xy * dx + o.precision_yy * dy));
                for (int b = 0; b < 3; ++b)
                    normal[a * 3 + b] += weight * (jx[a] * (o.precision_xx * jx[b] + o.precision_xy * jy[b]) +
                                                   jy[a] * (o.precision_xy * jx[b] + o.precision_yy * jy[b]));
            }
        }
        const double diagonal = std::max({normal[0], normal[4], normal[8]});
        if (!(diagonal > 0) || !std::isfinite(diagonal)) return false;
        for (int i = 0; i < 9; ++i) normal[i] /= diagonal;
        normal[0] += 1e-9; normal[4] += 1e-9; normal[8] += 1e-9;
        bool invertible;
        const auto inverse = sfm::inverse3(normal, &invertible);
        if (!invertible) return false;
        const auto delta = sfm::mul(inverse, sfm::Vec3{-gradient[0] / diagonal, -gradient[1] / diagonal, -gradient[2] / diagonal});
        if (!finite(delta)) return false;
        if (delta.norm() < 1e-10 * std::max(1.0, (point - observations[indices[0]].view->center).norm())) break;
        const double old_cost = cost(observations, indices, point, huber, check);
        bool improved = false;
        for (int trial = 0; trial < 12; ++trial) {
            const sfm::Vec3 candidate = point + delta * std::ldexp(1.0, -trial);
            if (cost(observations, indices, candidate, huber, check) < old_cost) {
                point = candidate; improved = true; break;
            }
        }
        if (!improved) break;
    }
    return finite(point);
}

void set_index(std::vector<uint32_t>& values, size_t index, uint32_t value) { values[index] = value; }
void set_index(DiskArray<uint32_t>& values, size_t index, uint32_t value) { values.set(index,value); }

template<class Observations, class Indices>
bool finish_refinement(const sfm::Vec3& initial, const Observations& observations, Indices& indices,
                       const GeometryOptions& options, PointEstimate& result, const std::function<void()>& check = {}) {
    if (indices.size() < (size_t)options.min_source_images) return false;
    sfm::Vec3 point = initial;
    for (int pass = 0; pass < 3; ++pass) {
        if (!optimize(observations,indices,options.max_reprojection_error,point,check)) return false;
        const auto before = indices.size(); size_t count = 0;
        for (size_t i = 0; i < before; ++i) {
            if (check) check();
            const auto index = indices[i];
            if (error(observations[index],point) <= options.max_reprojection_error) set_index(indices,count++,index);
        }
        indices.resize(count);
        if (indices.size() < (size_t)options.min_source_images) return false;
        if (indices.size() == before) break;
    }
    double max_angle = 0, max_error = 0;
    for (size_t i = 0; i < indices.size(); ++i) {
        if (check) check();
        max_error = std::max(max_error,error(observations[indices[i]],point));
        for (size_t j = 0; j < i; ++j) {
            if (check) check();
            const double angle = sfm::triangulationAngle(point,observations[indices[i]].view->center,
                                                        observations[indices[j]].view->center) * 57.29577951308232;
            max_angle = std::max(max_angle,std::min(angle,180 - angle));
        }
    }
    if (max_error > options.max_reprojection_error || max_angle < options.min_angle_degrees) return false;
    result.position = point; result.max_reprojection_error = max_error;
    return true;
}

}  // namespace

void View::validate() const {
    if (source_image < 0 || camera.width <= 0 || camera.height <= 0 ||
        (!source_camera && (camera.model != (int)CameraModelType::PINHOLE || camera.tier != (int)CameraDistortionType::None || camera.source_model >= 0)) ||
        camera.model < 0 || camera.model > (int)CameraModelType::EQUIRECTANGULAR || camera.tier < 0 || camera.tier > (int)CameraDistortionType::ThinPrism ||
        !std::isfinite(camera.fx) || !std::isfinite(camera.fy) || camera.fx <= 0 || camera.fy <= 0 ||
        !std::isfinite(camera.cx) || !std::isfinite(camera.cy) || !finite(center) ||
        !(grid_scale[0] > 0) || !(grid_scale[1] > 0) || !std::isfinite(grid_scale[0]) || !std::isfinite(grid_scale[1]))
        throw std::runtime_error("dense geometry requires a valid calibrated view");
    for (float value : camera.dist) if (!std::isfinite(value)) throw std::runtime_error("invalid dense lens distortion");
    for (float value : camera.source_params) if (!std::isfinite(value)) throw std::runtime_error("invalid dense source calibration");
    const auto identity = sfm::mul(world_to_camera, sfm::transpose(world_to_camera));
    for (int i = 0; i < 9; ++i)
        if (!std::isfinite(world_to_camera[i]) || std::fabs(identity[i] - (i % 4 == 0 ? 1.0 : 0.0)) > 1e-4)
            throw std::runtime_error("dense view rotation must be orthonormal");
    if (sfm::det3(world_to_camera) < 0.9999) throw std::runtime_error("dense view rotation reverses handedness");
}

void GeometryOptions::validate() const {
    if (!std::isfinite(min_angle_degrees) || min_angle_degrees <= 0 || min_angle_degrees >= 180 ||
        !std::isfinite(max_reprojection_error) || max_reprojection_error <= 0 ||
        !std::isfinite(max_relative_depth_error) || max_relative_depth_error <= 0 || max_relative_depth_error > 1 || min_source_images < 2)
        throw std::runtime_error("invalid dense geometry filtering settings");
}

bool bearing(const View& view, const sfm::Vec2& pixel, sfm::Vec3& ray) {
    double r[3];
    const double px[2] = {pixel.x, pixel.y};
    if (!camhost::pixel_ray(view.camera, px, r)) return false;
    ray = {r[0], r[1], r[2]};
    return finite(ray);
}

bool valid_observation(const Observation& observation) {
    sfm::Vec3 ray;
    return valid(observation) && bearing(*observation.view, observation.pixel, ray);
}

double depth(const View& view, const sfm::Vec3& point, const sfm::Vec3* reference_ray) {
    const auto q = camera_point(view, point);
    return view.source_camera ? reference_ray ? q.dot(*reference_ray) : q.norm() : q.z;
}

sfm::Vec2 pixel_residual(const View& view, const sfm::Vec2& a, const sfm::Vec2& b) {
    const double pa[2] = {a.x, a.y}, pb[2] = {b.x, b.y}; double delta[2];
    camhost::pixel_difference(view.camera, pa, pb, delta);
    return {delta[0], delta[1]};
}

double residual_length(const View& view, const sfm::Vec2& residual) {
    return std::hypot(residual.x * view.grid_scale[0], residual.y * view.grid_scale[1]);
}

double reprojection_error(const Observation& observation, const sfm::Vec3& point) { return error(observation, point); }

bool project(const View& view, const sfm::Vec3& point, sfm::Vec2& pixel) {
    const auto q = camera_point(view, point);
    if (!finite(q) || !(q.norm() > 0) || (!view.source_camera && q.z <= 0)) return false;
    const double ray[] = {q.x, q.y, q.z};
    double p[2];
    if (!camhost::ray_in_frame(view.camera, ray, p)) return false;
    pixel = {p[0], p[1]};
    return std::isfinite(pixel.x) && std::isfinite(pixel.y);
}

bool triangulate(const Observation& a, const Observation& b, const GeometryOptions& options, sfm::Vec3& point) {
    if (!valid(a) || !valid(b) || a.view->source_image == b.view->source_image ||
        (a.view->center - b.view->center).norm() == 0) return false;
    sfm::Vec3 ba, bb;
    if (!bearing(*a.view, a.pixel, ba) || !bearing(*b.view, b.pixel, bb)) return false;
    const sfm::Vec3 origin = a.view->center;
    sfm::Vec3 candidate;
    if (a.view->source_camera || b.view->source_camera) {
        const auto ra = sfm::mul(sfm::transpose(a.view->world_to_camera), ba);
        const auto rb = sfm::mul(sfm::transpose(b.view->world_to_camera), bb);
        const auto baseline = b.view->center - origin;
        const double dot = ra.dot(rb), determinant = 1 - dot * dot;
        if (!(determinant > 1e-12)) return false;
        const double da = (ra.dot(baseline) - dot * rb.dot(baseline)) / determinant;
        const double db = (dot * ra.dot(baseline) - rb.dot(baseline)) / determinant;
        if (!(da > 0) || !(db > 0)) return false;
        candidate = origin + (ra * da + baseline + rb * db) * 0.5;
    } else candidate = origin + sfm::triangulateDLT(projection(*a.view, origin), projection(*b.view, origin), ba, bb);
    if (!finite(candidate) || depth(*a.view, candidate) <= 0 || depth(*b.view, candidate) <= 0) return false;
    const double angle = sfm::triangulationAngle(candidate, a.view->center, b.view->center) * 57.29577951308232;
    if (std::min(angle, 180 - angle) < options.min_angle_degrees || error(a, candidate) > options.max_reprojection_error ||
        error(b, candidate) > options.max_reprojection_error) return false;
    point = candidate;
    return true;
}

bool refine_point(const sfm::Vec3& initial, const std::vector<Observation>& observations,
                    const GeometryOptions& options, PointEstimate& result) {
    if (!finite(initial) || observations.size() > std::numeric_limits<uint32_t>::max()) return false;
    std::map<int64_t, std::pair<double, uint32_t>> unique;
    for (uint32_t i = 0; i < observations.size(); ++i) {
        const auto& o = observations[i];
        if (!valid(o)) continue;
        const double residual = error(o, initial);
        if (residual > options.max_reprojection_error * 4) continue;
        auto found = unique.find(o.view->source_image);
        if (found == unique.end() || residual < found->second.first) unique[o.view->source_image] = {residual, i};
    }
    std::vector<uint32_t> indices;
    for (const auto& entry : unique) indices.push_back(entry.second.second);
    if (!finish_refinement(initial,observations,indices,options,result)) return false;
    result.observations = std::move(indices);
    return true;
}

bool refine_unique_point(const sfm::Vec3& initial, const DiskArray<Observation>& observations,
                         const GeometryOptions& options, DiskArray<uint32_t>& retained, PointEstimate& result,
                         const std::function<void()>& check) {
    retained.clear(); result = {};
    if (!finite(initial) || observations.size() > std::numeric_limits<uint32_t>::max()) return false;
    int64_t previous = -1;
    for (uint32_t i = 0; i < observations.size(); ++i) {
        if (check) check();
        const auto o = observations[i];
        if (!o.view || o.view->source_image <= previous) throw std::runtime_error("dense refinement observations must have sorted distinct source IDs");
        previous = o.view->source_image;
        if (valid(o) && error(o,initial) <= options.max_reprojection_error * 4) retained.push_back(i);
    }
    if (!finish_refinement(initial,observations,retained,options,result,check)) return false;
    return true;
}

bool depth_consistent(const View& reference, const sfm::Vec3& a, const sfm::Vec3& b, double max_relative_error,
                        const sfm::Vec3* reference_ray) {
    sfm::Vec3 ray;
    if (reference.source_camera && !reference_ray) { ray = camera_point(reference, a).normalized(); reference_ray = &ray; }
    const double da = depth(reference, a, reference_ray), db = depth(reference, b, reference_ray);
    return std::isfinite(da) && std::isfinite(db) && da > 0 && db > 0 &&
        std::fabs(da - db) <= max_relative_error * std::min(da, db);
}

}  // namespace spirula::dense
