#pragma once

// A surface from a dense point cloud and the cameras that saw it: Delaunay
// tetrahedra labelled inside/outside by an s-t min cut of the visibility rays
// (Labatut et al. 2009, Jancosek & Pajdla 2011). docs/meshlab.md, "mesh-dense".
// Points and cameras are in the dataset's file frame.

#include "mesh/MeshExport.h"
#include "mesh/MeshViews.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace meshing {

struct GraphCutConfig {
    int smooth_k = 40;           // smooth_points: neighbours per plane; 0 = off
    int smooth_iters = 3;
    double voxel = 0;            // thin to one point per voxel; 0 = off
    int64_t max_points = 3000000;// thin further until at most this many remain
    int max_rays = 8;            // views kept per point, the most frontal first
    int grid = 200;              // visibility depth grid, cells across the longer side
    double depth_eps = 0.03;     // relative depth tolerance of that test
    float alpha_vis = 1.0f;      // per crossed facet, per ray
    float sigma = 2.0f;          // sink offset behind a point, in local spacings
    float quality = 0.1f;        // facet-shape term, relative to alpha_vis
    float hull = 1.0f;           // source link of a convex-hull facet, x alpha_vis
    float long_edge = 6.0f;      // drop a face with an edge past this many spacings; <= 0 off
    int floater_min_faces = 100;
    float fill_hole_ratio = 0.02f;
    int fill_hole_max_edges = 48;  // long-face removal leaves holes of a few dozen edges
    float degenerate_angle_deg = 2.0f;
    int quality_iters = 2;
    int threads = 0;
    bool verbose = true;
};

struct GraphCutStats {
    int64_t input_points = 0, points = 0, rays = 0, tets = 0, inside = 0;
    int64_t relabelled = 0;      // tets moved outside to make every edge manifold
    int64_t faces = 0, long_faces = 0;
    double flow = 0;
};

// `rgb` may be empty. `progress(stage)` names each step as it starts.
MeshData graph_cut_mesh(const std::vector<double>& xyz, const std::vector<uint8_t>& rgb,
                        const ViewSet& views, const GraphCutConfig& cfg, GraphCutStats& stats,
                        const std::function<void(const char*)>& progress = {});

// Each point moved onto the Gaussian-weighted PCA plane of its smooth_k nearest
// neighbours, smooth_iters times: the slightly offset layers that different
// image pairs triangulate merge into one surface instead of folding the mesh.
void smooth_points(std::vector<double>& xyz, const GraphCutConfig& cfg);

// Per vertex, the colour of the nearest of `xyz` (the unthinned cloud).
void color_from_points(MeshData& mesh, const std::vector<double>& xyz,
                       const std::vector<uint8_t>& rgb, int threads);

}  // namespace meshing
