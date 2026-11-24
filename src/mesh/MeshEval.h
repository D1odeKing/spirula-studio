#pragma once

// Geometric accuracy of a mesh against a reference mesh or point cloud: the
// quick inner-loop score behind `spirula mesh-eval`. The numbers a paper
// reports come from each benchmark's official evaluator (docs/meshlab.md);
// this one only has to agree with them in direction.

#include "mesh/MeshExport.h"
#include "mesh/MeshParallel.h"

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace meshing {

struct EvalReference {
    MeshData mesh;                 // faces present: a reference surface
    std::vector<double> points;    // [N,3]; for a mesh, filled by sampling it
    std::vector<float> normals;    // [N,3] of a point reference, estimated on first use
    bool is_mesh() const { return !mesh.F.empty(); }
};

// A mesh when the file has faces, else its points. False with `error` set.
bool load_reference(const std::string& path, EvalReference& out, std::string& error);

struct EvalOptions {
    int64_t samples = 1000000;     // area-uniform points drawn from each surface
    std::vector<double> taus;      // F1 tolerances, dataset units
    double max_dist = std::numeric_limits<double>::infinity();   // DTU-style outlier cut
    bool crop = false;
    double crop_lo[3] = {0, 0, 0}, crop_hi[3] = {0, 0, 0};
    uint32_t seed = 1;
    int threads = 0;
};

struct F1Row { double tau = 0, precision = 0, recall = 0, f1 = 0; };

struct EvalResult {
    double acc_mean = 0, acc_median = 0;     // mesh -> reference
    double comp_mean = 0, comp_median = 0;   // reference -> mesh
    double chamfer = 0;                      // (acc_mean + comp_mean) / 2
    int64_t mesh_points = 0, ref_points = 0;
    std::vector<F1Row> f1;
    std::vector<float> vertex_error;         // per mesh vertex, distance to the reference
};

EvalResult evaluate_mesh(const MeshData& mesh, EvalReference& ref, const EvalOptions& opt);

// Rigid point-to-plane ICP of the mesh's vertices onto the reference,
// starting from M (row-major 3x4) and refining it in place. Returns the final
// trimmed RMS residual.
double icp_point_to_plane(const MeshData& mesh, EvalReference& ref, double M[12],
                          int iterations, int threads);

struct MeasurementPair {
    std::string name;
    double p1[3] = {0, 0, 0}, p2[3] = {0, 0, 0};
    double truth = 0;
    double measured = 0, snapped = 0;   // |p1 p2|, and between their closest mesh points
};

// {"pairs": [{"name", "p1": [x,y,z], "p2": [...], "true_distance": d}]} or the bare array.
bool read_measurements(const std::string& path, std::vector<MeasurementPair>& out, std::string& error);
void measure_pairs(const MeshData& mesh, std::vector<MeasurementPair>& pairs);

// Blue (0) -> red (`clip` and beyond), for an error PLY.
std::array<unsigned char, 3> error_color(float e, float clip);

}  // namespace meshing
