#pragma once

// The files `spirula mesh-dense` writes for a mesh job (docs/meshlab.md).
// Header-only: MeshJob.cpp needs it, and so do tests that link no GUI code.

#include "app/gui/MeshJob.h"
#include "mesh/MeshExport.h"

#include <string>
#include <vector>

namespace gui {

// One file per format, with the vertex colour only where the format holds it.
inline std::vector<std::string> meshlab_dense_outputs(const MeshJob& job) {
    const auto mode = job.colors[1] ? meshing::MeshColorMode::Vertex : meshing::MeshColorMode::None;
    const std::string base = meshing::mesh_output_strip_ext(job.output);
    std::vector<std::string> out;
    for (int i = 0; i < kNumMeshFormats; ++i) {
        if (!job.formats[i]) continue;
        const meshing::MeshFormatSpec spec = meshing::parse_one_mesh_format(kMeshFormats[i]);
        const bool fits = meshing::check_export_support(spec, mode).empty();
        out.push_back(meshing::mesh_output_paths(spec, fits ? mode : meshing::MeshColorMode::None, base).front());
    }
    if (out.empty()) out.push_back(base + ".ply");
    return out;
}

}  // namespace gui
