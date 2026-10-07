#pragma once

#include "dense/Geometry.h"
#include "dense/PairSelection.h"
#include "roma/Roma.h"

#include <optional>
#include <string>

namespace spirula::dense {

struct DenseConfig {
    roma::MatchOptions match;
    PairOptions pairs;
    GeometryOptions geometry;
    std::string preset = "precise", checkpoint = "romav2.0.1";
    std::string image_dir = "images", recon_dir, pair_list;
    std::string mask_dir = "masks", feature_mask_dir = "feature_masks";
    bool use_masks = true;
    bool training_masks = true, alpha_masks = true, feature_masks = true;
    bool invert_masks = false;
    double mask_threshold = 0.5;
    int mask_boundary = 0;
    std::string exif_orientation = "none";
    std::string metashape_xml, metashape_psx;
    int metashape_component = -1;
    bool split_views = true;
    bool sparse_face_pairs = true;
    int max_face_size = 1280;
    std::string matching_space = "rectified";
    double reference_fraction = 0.8, source_reprojection_error = 1;   // matcher cells
    uint64_t samples_per_reference = 10000, sampling_seed = 0;
    int stride = 1;
    uint64_t point_limit = 0;
    double min_overlap = 0.5, max_cycle_error = 1;
    bool cycle_check = true;
    double voxel_size = 0;
    bool remove_outliers = false;
    int outlier_neighbors = 16;
    double outlier_stddev = 2;
    uint64_t image_cache_bytes = 0;
    int cpu_workers = 0;
    bool resume = true, rebuild = false, keep_cache = true;
    std::string device, image_gamut, image_exposure;
    std::optional<bool> image_is_linear;
    void apply_preset(const std::string& name);
    void apply_source_workflow();
    GeometryOptions resolved_geometry() const;
    void validate() const;
    void validate_run() const;
    uint64_t resolved_image_cache_bytes() const;
    bool effective_cycle_check() const { return cycle_check && match.bidirectional; }
};

}  // namespace spirula::dense
