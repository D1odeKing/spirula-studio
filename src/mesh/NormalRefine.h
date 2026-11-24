#pragma once

// A mesh's surface reshaped by monocular normal maps while its position stays
// on measured data: guide normals from every view that sees a face, a guided
// bilateral normal filter (Zheng et al. 2011), then a vertex solve anchored to
// the input (or the dense cloud) under a hard displacement cap. File frame.
// docs/meshlab.md, "mesh-refine".

#include "mesh/MeshExport.h"
#include "mesh/MeshViews.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace meshing {

struct RefineConfig {
    int raster = 640;            // z-buffer size, longer side, pixels
    float outlier_deg = 30.0f;   // a view this far from the median guide is dropped
    float pull = 0.7f;           // how far a fully confident face moves to its guide
    float sigma_r = 0.3f;        // range kernel, |n_i - n_j|; creases survive past it
    float sigma_s = 2.0f;        // spatial kernel, in mean edge lengths
    int normal_iters = 8;
    int vertex_iters = 15;
    int rounds = 2;              // filter + vertex solve, repeated
    float lambda = 0.05f;        // anchor weight against the normal term (areas normalized)
    double max_shift = 0;        // hard cap per vertex, file units; 0 = one mean edge
    bool plane_snap = false;
    float plane_deg = 4.0f;      // region growth: guide within this of the region's
    double plane_tol = 0;        // snap when the anchors' plane RMS is under this; 0 = max_shift / 4
    int plane_min_faces = 200;
    int threads = 0;
};

struct RefineStats {
    int views = 0;                 // views that had a normal map
    int64_t guided = 0;            // faces with a guide normal
    double mean_conf = 0;
    double guide_angle_deg = 0;     // mean angle between a guide and its input face
    double mean_shift = 0, max_shift = 0, cap = 0;
    int planes = 0;
    int64_t snapped = 0;
};

// `anchors` (dense points, [N,3]) may be empty: the input vertices anchor
// themselves. Per vertex, `shift` gets the signed move along its normal.
void refine_with_normals(MeshData& mesh, const ViewSet& views, const std::vector<double>& anchors,
                         const RefineConfig& cfg, RefineStats& stats, std::vector<float>& shift,
                         const std::function<void(const char*)>& progress = {});

// The guide alone, for a test or a caller with normal maps in memory: per
// face, the robust mean of the samples and its confidence in [0, 1].
struct GuideSample { int32_t face; float n[3]; float w; };
void robust_guides(int64_t faces, const std::vector<GuideSample>& samples, float outlier_deg,
                   std::vector<float>& guide, std::vector<float>& conf);

// Every visible face's mean map normal in one view, turned to the file frame
// and toward the camera. `map` stands for the view's normal image.
void guide_samples_for_view(const MeshData& mesh, const MeshView& view, const NormalMap& map,
                            int raster, std::vector<GuideSample>& out);

}  // namespace meshing
