#pragma once

// A dataset's cameras and normal maps in its FILE frame: the frame
// dense/roma.ply is written in and every mesh-lab tool reads and writes
// (docs/meshlab.md). A splat mesh is in its run's training frame instead;
// train_to_file() is the map between the two.

#include "data/CameraMath.h"
#include "mesh/MeshExport.h"

#include <array>
#include <string>
#include <vector>

namespace meshing {

struct MeshView {
    std::string image, normal, depth;  // absolute; a map is "" when the view has none
    camhost::Camera cam;           // at the camera record's own resolution
    double R[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // file frame -> CV camera, row-major
    double C[3] = {0, 0, 0};       // centre, file frame
    double unit = 1.0;             // camera-space length of one file-frame unit
};

struct ViewSet {
    std::vector<MeshView> views;
    std::array<double, 16> raw_to_file{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    bool metric = false;           // gauge.txt says one unit is one metre
};

struct ViewOptions {
    // Relative to the dataset, or absolute.
    std::string recon_dir, image_dir = "images", normal_dir = "normals", depth_dir = "depths";
};

// Every registered camera, sorted by image name. Throws when there are none.
ViewSet load_views(const std::string& dataset, const ViewOptions& opt = {});

// Camera space (CV: +Z forward, +Y down) of a file-frame point, in file units.
void to_camera(const MeshView& v, const double X[3], double out[3]);
// File-frame point -> pixel of the camera record; false off-frame or behind.
bool project(const MeshView& v, const double X[3], double px[2], double* depth = nullptr);
// A camera-space direction turned into the file frame (unit length kept).
void camera_dir_to_file(const MeshView& v, const double c[3], double out[3]);
// Unit ray through `px`, file frame. False outside the lens' valid domain.
bool pixel_ray_file(const MeshView& v, const double px[2], double dir[3]);

struct NormalMap {
    int width = 0, height = 0;
    std::vector<float> n;          // [h*w*3] unit CV-camera normals; 0 where invalid
    // Nearest texel under a pixel of the camera record: a map at another size
    // stands for the same lens scaled.
    bool sample(const MeshView& v, const double px[2], float out[3]) const;
};

// 8-bit PNG/JPG, n = byte/127.5 - 1, invalid where the trainer's
// gt_normal_valid says so (black). False and `error` set when it does not load.
bool load_normal_map(const std::string& path, NormalMap& out, std::string& error);

// A run's training frame -> the dataset's file frame, row-major 3x4, from the
// scene_transform.json beside its config.json. False when there is none.
bool train_to_file(const std::string& run_dir, const ViewSet& views, double M[12]);

void apply_affine(const double M[12], MeshData& mesh);
// False when M is singular.
bool invert_affine(const double M[12], double out[12]);
bool read_matrix_file(const std::string& path, double M[12], std::string& error);

}  // namespace meshing
