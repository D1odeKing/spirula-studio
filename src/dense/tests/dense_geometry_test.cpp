#include "dense/Geometry.h"
#include "core/CameraModel.h"
#include "data/SourceCamera.h"
#include "dense/DiskArray.h"

#include <cmath>
#include <chrono>
#include <algorithm>
#include <array>
#include <vector>
#include <cstdio>
#include <stdexcept>

namespace {

using namespace spirula::dense;
using sfm::Vec3;

void check(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}

View view(int64_t id, const Vec3& center) {
    View out;
    out.source_image = id;
    out.center = center;
    out.camera.width = 320; out.camera.height = 240;
    out.camera.fx = out.camera.fy = 200;
    out.camera.cx = 160; out.camera.cy = 120;
    out.validate();
    return out;
}

Observation observe(const View& camera, const Vec3& point) {
    Observation out;
    out.view = &camera;
    check(project(camera, point, out.pixel), "test point not visible");
    return out;
}

void test_plane() {
    const View a = view(0, {0, 0, 0}), b = view(1, {1, 0, 0});
    GeometryOptions options;
    for (int y = -10; y <= 10; ++y)
        for (int x = -10; x <= 10; ++x) {
            const Vec3 expected{x / 10.0, y / 10.0, 5 + x / 100.0 + y / 200.0};
            Vec3 actual;
            const auto oa = observe(a, expected), ob = observe(b, expected);
            check(triangulate(oa, ob, options, actual), "valid plane point rejected");
            check((actual - expected).norm() < 1e-8, "plane point triangulates incorrectly");
            Vec3 ray;
            check(bearing(a, oa.pixel, ray), "pixel bearing rejected");
            check((ray - expected.normalized()).norm() < 1e-12, "pixel-center convention differs");
        }
    const Vec3 expected{0.5, -0.2, 5};
    Vec3 point;
    auto oa = observe(a, expected), ob = observe(b, expected);
    const View stationary = view(2, {0, 0, 0});
    check(!triangulate(oa, observe(stationary, expected), options, point), "zero baseline accepted");
    View duplicate = b; duplicate.source_image = a.source_image;
    check(!triangulate(oa, observe(duplicate, expected), options, point), "two faces counted as separate images");
    const View tiny = view(3, {0.001, 0, 0});
    check(!triangulate(oa, observe(tiny, expected), options, point), "tiny parallax accepted");
    ob.pixel.x = 220;
    check(!triangulate(oa, ob, options, point), "behind-camera point accepted");
    ob = observe(b, expected); ob.pixel.y += 12;
    check(!triangulate(oa, ob, options, point), "bad reprojection accepted");
    ob = observe(b, expected); ob.pixel.x = -0.1;
    check(!triangulate(oa, ob, options, point), "out-of-frame observation accepted");
    check(depth_consistent(a, expected, {0.5, -0.2, 5.04}, 0.01), "consistent depth rejected");
    check(!depth_consistent(a, expected, {0.5, -0.2, 5.1}, 0.01), "depth discontinuity accepted");
    std::printf("PASS analytic tilted plane, pixel centers, geometry filters\n");
}

void test_frame_precision() {
    const Vec3 origin{4000000.123, -3000000.456, 2000000.789};
    const Vec3 expected = origin + Vec3{0.5, -0.2, 5};
    View a = view(0, origin), b = view(1, origin + Vec3{1, 0, 0});
    const double angle = 0.2;
    b.world_to_camera = {std::cos(angle),0,std::sin(angle), 0,1,0, -std::sin(angle),0,std::cos(angle)};
    b.validate();
    Vec3 actual;
    check(triangulate(observe(a, expected), observe(b, expected), {}, actual), "georeferenced point rejected");
    check((actual - expected).norm() < 1e-8, "triangulation lost source-frame precision");
    std::printf("PASS rotated camera and large source coordinates\n");
}

void test_multi_view() {
    const Vec3 expected{0.3, -0.2, 5};
    std::vector<View> views;
    for (int i = 0; i < 7; ++i) views.push_back(view(i, {(i - 3) * 0.4, (i % 3 - 1) * 0.4, 0}));
    std::vector<Observation> observations;
    for (int i = 0; i < 7; ++i) {
        auto o = observe(views[i], expected);
        o.pixel.x += (i % 3 - 1) * 0.2;
        o.pixel.y += (i % 2 ? 1 : -1) * 0.1;
        o.precision_xx = 1.2; o.precision_xy = -0.1; o.precision_yy = 0.8;
        observations.push_back(o);
    }
    observations.back().pixel.x += 6;
    GeometryOptions options;
    options.validate();
    PointEstimate result;
    check(refine_point(expected + Vec3{0.01, -0.01, 0.02}, observations, options, result), "multi-view refinement rejected");
    check(result.observations.size() == 6, "multi-view outlier was not rejected");
    check((result.position - expected).norm() < 0.015, "multi-view refinement did not converge");
    check(result.max_reprojection_error < 0.4, "multi-view reprojection error is excessive");
    const auto directory = std::filesystem::temp_directory_path() /
        ("spirula-point-spill-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    {
        DiskArray<Observation> spilled(directory / "observations",sizeof(Observation));
        DiskArray<uint32_t> retained(directory / "indices",sizeof(uint32_t));
        for (const auto& observation : observations) spilled.push_back(observation);
        PointEstimate disk_result;
        check(refine_unique_point(expected + Vec3{0.01,-0.01,0.02},spilled,options,retained,disk_result),
              "spilled point refinement rejected valid geometry");
        check(retained.size() == result.observations.size() && (disk_result.position - result.position).norm() == 0 &&
              disk_result.max_reprojection_error == result.max_reprojection_error,"spilled point refinement changed geometry");
        for (size_t i = 0; i < retained.size(); ++i)
            check(retained[i] == result.observations[i],"spilled point refinement changed retained support");
        bool stopped = false; size_t checks = 0;
        try {
            refine_unique_point(expected + Vec3{0.01,-0.01,0.02},spilled,options,retained,disk_result,[&] {
                if (++checks > spilled.size()) throw std::runtime_error("cancelled");
            });
        } catch (const std::exception&) { stopped = true; }
        check(stopped && checks == spilled.size() + 1,"spilled optimization ignored cancellation after observation collection");
    }
    std::filesystem::remove(directory);
    std::vector<Observation> two{observations[0], observations[1]};
    check(!refine_point(expected, two, options, result), "minimum support was silently relaxed");
    two.push_back(two[0]);
    check(!refine_point(expected, two, options, result), "duplicate original image inflated support");
    options.min_source_images = 2;
    check(refine_point(expected, two, options, result) && result.observations.size() == 2, "two-view override rejected");
    auto invalid = observations;
    invalid[0].precision_xy = 10;
    options.min_source_images = 6;
    check(!refine_point(expected, invalid, options, result), "invalid precision supplied support");
    for (int i = 0; i < 7; ++i)
        check(views[i].center.x == (i - 3) * 0.4 && views[i].world_to_camera == sfm::mat3Identity(), "fixed cameras were modified");
    std::printf("PASS robust multi-view refinement, distinct-image support, precision, fixed cameras\n");
}

View wide_view(int64_t id, const Vec3& center, CameraModelType model) {
    auto out = view(id, center); out.source_camera = true;
    out.camera.model = (int)model; out.camera.width = 1024;
    out.camera.height = model == CameraModelType::EQUIRECTANGULAR ? 512 : 1024;
    out.camera.cx = 512; out.camera.cy = out.camera.height / 2;
    out.camera.fx = out.camera.fy = model == CameraModelType::EQUIRECTANGULAR ? 512 / 3.141592653589793 : 120;
    out.validate(); return out;
}

void test_source_cameras() {
    for (const auto model : {CameraModelType::PINHOLE, CameraModelType::FISHEYE, CameraModelType::EQUISOLID, CameraModelType::EQUIRECTANGULAR}) {
        std::vector<View> cameras;
        for (int i = 0; i < 4; ++i) cameras.push_back(wide_view(i, {i * 0.4, (i % 2) * 0.2, 0}, model));
        GeometryOptions options; options.max_reprojection_error = 0.08;
        for (const Vec3 expected : {Vec3{0.3,0.2,5}, Vec3{4,0.2,-1}, Vec3{0.02,0.1,-5}}) {
            if (model == CameraModelType::PINHOLE && expected.z < 0) continue;
            if (model == CameraModelType::EQUISOLID && expected.x < 1 && expected.z < 0) continue;
            std::vector<Observation> observations;
            for (const auto& camera : cameras) observations.push_back(observe(camera, expected));
            Vec3 actual;
            check(triangulate(observations[0], observations[1], options, actual), "valid source rays rejected");
            check((actual - expected).norm() < 1e-7, "source rays triangulate incorrectly");
            PointEstimate refined;
            check(refine_point(expected + Vec3{0.0002,-0.0001,0.0001}, observations, options, refined), "source refinement rejected");
            check((refined.position - expected).norm() < 1e-7, "source refinement did not converge");
            auto wrong = observations[1];
            sfm::Vec2 n, f;
            check(project(cameras[1],expected * 0.95,n) && project(cameras[1],expected * 1.05,f), "epipolar fixture not visible");
            const auto tangent = pixel_residual(cameras[1],f,n);
            const double length = std::hypot(tangent.x,tangent.y);
            check(length > 0, "epipolar fixture degenerate");
            wrong.pixel.x += -tangent.y / length * 2; wrong.pixel.y += tangent.x / length * 2;
            check(!triangulate(observations[0], wrong, options, actual), "source pixel tolerance ignored");
        }
    }
    auto panorama = wide_view(0, {}, CameraModelType::EQUIRECTANGULAR);
    const Vec3 forward{0,0,1};
    check(depth(panorama, {1,0,-3}, &forward) < 0, "source depth ignored the observed ray direction");
    check(!depth_consistent(panorama, {0,0,4}, {4,0,0}, 0.01), "source depth compared range instead of distance along the reference ray");
    const auto delta = pixel_residual(panorama, {0.02,256}, {1023.98,256});
    check(std::abs(delta.x - 0.04) < 1e-10, "panorama residual does not wrap");
    Vec3 seam_a, seam_b;
    check(camhost::full_longitude(panorama.camera) && bearing(panorama,{-0.25,256},seam_a) &&
          bearing(panorama,{1023.75,256},seam_b) && (seam_a - seam_b).norm() < 1e-12,
          "unwrapped panorama pixels did not produce the same seam ray");
    auto cropped = panorama.camera; cropped.width = 256; cropped.cx = 128;
    double outside[2] = {-1,256};
    check(!camhost::full_longitude(cropped) && !camhost::normalize_pixel(cropped,outside),
          "a cropped panorama was treated as a periodic image");
    auto lens = wide_view(1, {}, CameraModelType::FISHEYE);
    Vec3 ray;
    check(!bearing(lens, {1024,1024}, ray), "invalid fisheye domain accepted");
    auto perspective = wide_view(2, {}, CameraModelType::PINHOLE);
    sfm::Vec2 pixel;
    check(!project(perspective, {1,0,-3}, pixel), "source pinhole sees behind camera");
    auto original = view(3, {}); original.source_camera = true;
    original.camera.source_model = srccam::kColmapFullOpenCV;
    const float params[16] = {200,200,160,120, .04f,-.001f,.001f,-.002f,0,.01f,0,0};
    std::copy_n(params, 16, original.camera.source_params);
    const Vec3 expected{0.3,0.2,1};
    check(project(original, expected, pixel) && bearing(original, pixel, ray), "original source lens inversion failed");
    check((ray - expected.normalized()).norm() < 1e-8, "inverse uses fitted lens instead of source lens");
    const double q[3] = {0.3,0.2,1}; double j[6];
    check(camhost::projection_jacobian(perspective.camera, q, j), "source projection derivative failed");
    check(std::abs(j[0] - perspective.camera.fx) < 1e-6 && std::abs(j[2] + perspective.camera.fx * q[0]) < 1e-6,
          "numerical projection derivative differs from analytic pinhole");
    std::printf("PASS source lenses, spherical cheirality, wrapped residuals, refinement and source precision\n");
}

// Every inner pixel must invert and re-project, for each model, tier and fitted source lens.
void test_camera_matrix() {
    const float opencv[8] = {-.05f,.01f,.001f,-.0005f};
    const float thin_prism[8] = {-.03f,.004f,-.0005f,.0001f,.0008f,-.0004f,.0003f,-.0002f};
    struct Lens { const char* name; int model, tier, source; std::array<float,16> params; };
    std::vector<Lens> lenses;
    for (int model = 0; model <= (int)CameraModelType::EQUIRECTANGULAR; ++model)
        for (int tier = 0; tier <= (model == (int)CameraModelType::EQUIRECTANGULAR ? 0 : 2); ++tier)
            lenses.push_back({"model x tier", model, tier, -1, {}});
    lenses.push_back({"FOV", 0, 0, srccam::kColmapFOV, {200,200,160,120,.9f}});
    lenses.push_back({"SIMPLE_DIVISION", 0, 0, srccam::kColmapSimpleDivision, {200,160,120,-.1f}});
    lenses.push_back({"EUCM", 1, 0, srccam::kColmapEUCM, {200,200,160,120,.6f,1.1f}});
    lenses.push_back({"FISHEYE624", 1, 0, srccam::kColmapRadTanThinPrismFisheye,
                      {200,200,160,120,.02f,-.01f,0,0,0,0,.0005f,-.0003f,.0002f,0,0,.0001f}});
    lenses.push_back({"skewed fisheye", 1, 0, srccam::kSkewed,
                      {200,200,160,120,.5f,.02f,-.003f,0,0,.0005f,-.0002f,0,0,srccam::kSkewBaseFisheye,srccam::kSkewRadialPolynomial}});
    for (const auto& lens : lenses) {
        View camera = view(0, {}); camera.source_camera = true;
        camera.camera.model = lens.model; camera.camera.tier = lens.tier;
        if (lens.tier) std::copy_n(lens.tier == 1 ? opencv : thin_prism, 8, camera.camera.dist);
        if (lens.model == (int)CameraModelType::EQUIRECTANGULAR) {
            camera.camera.width = 640; camera.camera.height = 320; camera.camera.cx = 320; camera.camera.cy = 160;
            camera.camera.fx = camera.camera.fy = 320 / 3.141592653589793;
        }
        camera.camera.source_model = lens.source;
        std::copy_n(lens.params.data(), 16, camera.camera.source_params);
        camera.validate();
        const double rx = 0.4 * camera.camera.width, ry = 0.4 * camera.camera.height;
        for (int j = -7; j <= 7; ++j) for (int i = -7; i <= 7; ++i) {
            if (i * i + j * j > 49) continue;
            const sfm::Vec2 pixel{camera.camera.cx + rx * i / 7, camera.camera.cy + ry * j / 7};
            Vec3 ray; sfm::Vec2 back;
            if (!bearing(camera, pixel, ray) || !project(camera, ray * 3.0, back) ||
                residual_length(camera, pixel_residual(camera, back, pixel)) > 1e-3) {
                std::printf("lens %s model %d tier %d at (%g, %g)\n", lens.name, lens.model, lens.tier, pixel.x, pixel.y);
                throw std::runtime_error("an inner pixel of a supported camera did not round-trip");
            }
        }
    }

    // One tolerance in matcher cells serves a 2K photo and an 8K panorama alike.
    auto panorama = view(0, {}); panorama.source_camera = true;
    panorama.camera.model = (int)CameraModelType::EQUIRECTANGULAR;
    panorama.camera.width = 8192; panorama.camera.height = 4096; panorama.camera.cx = 4096; panorama.camera.cy = 2048;
    panorama.camera.fx = panorama.camera.fy = 4096 / 3.141592653589793;
    panorama.grid_scale[0] = 640.0 / 8192; panorama.grid_scale[1] = 480.0 / 4096;
    panorama.validate();
    auto photo = view(1, {0.5, 0, 0}); photo.source_camera = true;
    photo.camera.width = 2048; photo.camera.height = 1536; photo.camera.cx = 1024; photo.camera.cy = 768;
    photo.camera.fx = photo.camera.fy = 1600;
    photo.grid_scale[0] = 640.0 / 2048; photo.grid_scale[1] = 480.0 / 1536;
    photo.validate();
    const Vec3 point{0.2, 0.1, 4};
    Observation a = observe(panorama, point), b = observe(photo, point);
    GeometryOptions options; options.max_reprojection_error = 1;
    sfm::Vec2 n, f;
    check(project(photo, point * 0.95, n) && project(photo, point * 1.05, f), "scale fixture not visible");
    const auto tangent = pixel_residual(photo, f, n);
    const double length = std::hypot(tangent.x, tangent.y);
    auto shifted = b;
    shifted.pixel.x += -tangent.y / length * 0.8; shifted.pixel.y += tangent.x / length * 0.8;
    Vec3 actual;
    check(triangulate(a, shifted, options, actual), "sub-cell matcher noise rejected on a high-resolution panorama");
    shifted = b;
    shifted.pixel.x += -tangent.y / length * 12; shifted.pixel.y += tangent.x / length * 12;
    check(!triangulate(a, shifted, options, actual), "a multi-cell mismatch accepted");
    std::printf("PASS every camera model, distortion tier and fitted lens inverts; tolerance in matcher cells\n");
}

}  // namespace

int main() {
    try {
        test_plane();
        test_frame_precision();
        test_multi_view();
        test_source_cameras();
        test_camera_matrix();
    } catch (const std::exception& e) { std::printf("FAIL %s\n", e.what()); return 1; }
    return 0;
}
