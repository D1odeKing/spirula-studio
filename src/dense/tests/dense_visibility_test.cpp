#include "dense/PairSelection.h"
#include "dense/Visibility.h"
#include "core/CameraModel.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {

using namespace spirula::dense;
using sfm::Vec3;
constexpr double kPi = 3.141592653589793;

void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }

// A pinhole view at `center` looking along `forward`, with world up +z.
View look(int64_t source, Vec3 center, Vec3 forward, double fov_degrees, int width = 64, int height = 48) {
    View v;
    v.source_image = source; v.center = center;
    v.camera.model = (int)CameraModelType::PINHOLE;
    v.camera.width = width; v.camera.height = height;
    v.camera.fx = v.camera.fy = 0.5 * width / std::tan(0.5 * fov_degrees * kPi / 180);
    v.camera.cx = width / 2.0; v.camera.cy = height / 2.0;
    const Vec3 z = forward.normalized(), x = z.cross({0, 0, 1}).normalized(), y = z.cross(x);
    v.world_to_camera = {x.x, x.y, x.z, y.x, y.y, y.z, z.x, z.y, z.z};
    return v;
}

PairImage pair_image(const View& v, std::vector<uint64_t> visible) {
    PairImage image;
    image.source_image = v.source_image; image.name = std::to_string(v.source_image); image.center = v.center;
    image.forward = Vec3{v.world_to_camera[6], v.world_to_camera[7], v.world_to_camera[8]}.normalized();
    image.up = Vec3{v.world_to_camera[3], v.world_to_camera[4], v.world_to_camera[5]}.normalized();
    image.half_fov_radians = std::atan(0.5 * std::hypot(v.camera.width / v.camera.fx, v.camera.height / v.camera.fy));
    image.visible_points = std::move(visible);
    return image;
}

uint64_t plan(const std::vector<View>& views, const std::vector<Vec3>& points, int coverage, int neighbors) {
    auto projected = project_visibility(views, points);
    std::vector<PairImage> images;
    for (size_t v = 0; v < views.size(); ++v) images.push_back(pair_image(views[v], std::move(projected.visible[v])));
    PairOptions options;
    options.reference_coverage = coverage; options.neighbors = neighbors;
    return select_pairs(images, options, [](ImagePair) {}, {}, projected.points).emitted;
}

// Points on a sphere of radius 1 at the origin, deterministic.
std::vector<Vec3> statue() {
    std::vector<Vec3> points;
    for (int i = 0; i < 4000; ++i) {
        const double z = 1 - 2 * (i + 0.5) / 4000, r = std::sqrt(1 - z * z), a = i * 2.399963229728653;
        points.push_back({r * std::cos(a), r * std::sin(a), z});
    }
    return points;
}

std::vector<View> orbit(size_t n, double fov) {
    std::vector<View> views;
    for (size_t i = 0; i < n; ++i) {
        const double a = 2 * kPi * i / n;
        const Vec3 center{4 * std::cos(a), 4 * std::sin(a), 0.5};
        views.push_back(look((int64_t)i, center, Vec3{} - center, fov));
    }
    return views;
}

// Mutant: occlusion ignored, so the back of the sphere counts as seen.
void nearer_points_hide_farther_ones() {
    const auto points = statue();
    const auto views = orbit(1, 40);
    const auto projected = project_visibility(views, points);
    // The camera is 4.03 from the centre, so the far half lies beyond 4.03; the limb shares grid cells with
    // the near side within the tolerance, so only points well behind it are counted.
    size_t back = 0;
    for (uint64_t i : projected.visible[0]) back += (projected.points[i] - views[0].center).norm() > 4.3;
    require(!projected.visible[0].empty() && back * 20 < projected.visible[0].size(),
            "the far side of the sphere was counted as visible: " + std::to_string(back) + " of " +
            std::to_string(projected.visible[0].size()));
}

// Mutant: without points no view sees anything, so every view pairs by pose alone.
void rays_stand_in_without_points() {
    const auto views = orbit(24, 40);
    const auto projected = project_visibility(views, {});
    size_t seen = 0;
    for (const auto& visible : projected.visible) seen += !visible.empty();
    require(seen == views.size(), "views without sparse points saw nothing");
    double mean = 0;
    for (const auto& p : projected.points) mean += p.norm() / projected.points.size();
    require(mean < 2, "the ray points are not where the optical axes meet: mean radius " + std::to_string(mean));
}

// Mutant: pairs proportional to the number of images, as with a fixed neighbour count per image.
void ten_times_the_images_do_not_ten_times_the_pairs() {
    const auto points = statue();
    const uint64_t sparse = plan(orbit(36, 40), points, 8, 3), dense = plan(orbit(360, 40), points, 8, 3);
    require(sparse > 0 && dense < 2 * sparse,
            "36 views planned " + std::to_string(sparse) + " pairs, 360 views " + std::to_string(dense));
}

// Mutant: the pair count follows the number of images rather than the surface each one sees.
void a_few_wide_views_need_fewer_pairs_than_many_narrow_ones() {
    std::vector<Vec3> wall;
    for (int i = 0; i < 4000; ++i) {
        const double a = 2 * kPi * i / 4000, z = -1.5 + 3.0 * ((i * 37) % 4000) / 4000;
        wall.push_back({6 * std::cos(a), 6 * std::sin(a), z});
    }
    auto ring = [](size_t n, double fov) {
        std::vector<View> views;
        for (size_t i = 0; i < n; ++i) {
            const double a = 2 * kPi * i / n;
            const Vec3 center{0.5 * std::cos(a * 3), 0.5 * std::sin(a * 3), 0};
            views.push_back(look((int64_t)i, center, {std::cos(a), std::sin(a), 0}, fov));
        }
        return views;
    };
    const uint64_t wide = plan(ring(24, 90), wall, 8, 3), narrow = plan(ring(96, 35), wall, 8, 3);
    require(wide > 0 && narrow > wide, "24 wide views planned " + std::to_string(wide) + " pairs, 96 narrow ones " +
            std::to_string(narrow));
}

}  // namespace

int main() {
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
        {"nearer_points_hide_farther_ones", nearer_points_hide_farther_ones},
        {"rays_stand_in_without_points", rays_stand_in_without_points},
        {"ten_times_the_images_do_not_ten_times_the_pairs", ten_times_the_images_do_not_ten_times_the_pairs},
        {"a_few_wide_views_need_fewer_pairs_than_many_narrow_ones", a_few_wide_views_need_fewer_pairs_than_many_narrow_ones},
    };
    int failures = 0;
    for (const auto& test : tests) {
        try { test.run(); std::printf("PASS %s\n", test.name); }
        catch (const std::exception& e) { ++failures; std::printf("FAIL %s: %s\n", test.name, e.what()); }
    }
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
