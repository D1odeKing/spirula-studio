// MeshJob.cpp -- see MeshJob.h.

#include "app/gui/MeshJob.h"
#include "app/gui/MeshLabOutputs.h"

#include "mesh/MeshExport.h"

#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace gui {

namespace {

// Whichever of the requested formats carries the most: a textured GLB is
// self-contained, a glTF needs its sidecars, OBJ has no vertex colors, PLY has
// no texture and STL has no color at all. Preview quality follows that order.
const char* kPreviewOrder[] = {"glb", "gltf", "obj", "ply", "stl"};

std::vector<meshing::MeshOutputRequest> mesh_job_plan(const MeshJob& job) {
    std::vector<meshing::MeshFormatSpec> specs;
    for (const char* want : kPreviewOrder)
        for (int i = 0; i < kNumMeshFormats; ++i)
            if (job.formats[i] && std::strcmp(kMeshFormats[i], want) == 0) {
                meshing::MeshFormatSpec spec;
                spec.fmt = want;
                specs.push_back(spec);
            }
    // Richest colour first, so the first path the plan produces is the one the
    // preview wants. The per-colour suffix follows the SET, not the order, so
    // this cannot disagree with the child about where a file lands.
    std::vector<meshing::MeshColorMode> modes;
    for (int i = kNumMeshColorModes - 1; i >= 0; --i)
        if (job.colors[i]) modes.push_back((meshing::MeshColorMode)i);
    return meshing::plan_mesh_outputs(
        specs, modes, meshing::mesh_output_strip_ext(job.output));
}

}  // namespace


bool MeshJob::wants_color(int mode) const {
    return mode >= 0 && mode < kNumMeshColorModes && colors[mode];
}

std::string MeshJob::preview_path() const {
    const std::vector<std::string> all = mesh_job_outputs(*this);
    return all.empty() ? std::string() : all.front();
}

std::vector<std::string> mesh_job_outputs(const MeshJob& job) {
    std::vector<std::string> out;
    if (job.output.empty()) return out;
    if (job.source == 1) return meshlab_dense_outputs(job);
    for (const meshing::MeshOutputRequest& r : mesh_job_plan(job))
        out.push_back(r.base + "." + r.spec.fmt);
    return out;
}

bool mesh_job_writes_nothing(const MeshJob& job) {
    return mesh_job_plan(job).empty();
}

// A file becomes <name>_mesh, NEVER <name> -- meshing `splat.ply` to base
// `splat` writes `splat.ply`, i.e. over the model being meshed.
std::string default_mesh_output(const std::string& checkpoint) {
    if (checkpoint.empty()) return {};
    std::error_code ec;
    fs::path base(checkpoint);
    if (fs::is_regular_file(base, ec))
        base.replace_filename(base.stem().string() + "_mesh");
    else
        base /= "mesh";
    return base.string();
}

}  // namespace gui
