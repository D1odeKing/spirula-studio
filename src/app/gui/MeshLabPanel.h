#pragma once

// The mesh-lab half of the mesh screen (docs/meshlab.md): its controls, and
// the `spirula mesh-dense` / `spirula mesh-refine` commands MeshRunner runs
// for them. Kept out of MeshRunner.cpp and GuiApp.cpp so a merge from upstream
// meets a few call lines there and nothing else.

#include "app/gui/MeshJob.h"

#include <functional>
#include <string>
#include <vector>

namespace gui {

// The source / refine / max-shift controls.
void draw_meshlab_options(MeshJob& job);

// A texture is not offered: the dense mesher colours vertices, and refining
// moves vertices that a texture atlas has split along its seams.
bool meshlab_no_texture(const MeshJob& job);

// The dataset a job's cameras come from: its own field, else the `data` key of
// the run's config.json. "" when neither names one.
std::string meshlab_dataset(const MeshJob& job);

// For a dense-cloud source, `argv` (exe, --lang, code, then `mesh ...`) becomes
// the mesh-dense command. False when there is no dataset to mesh.
bool meshlab_dense_argv(const MeshJob& job, const std::string& formats, const std::string& colors,
                        std::vector<std::string>& argv);

// mesh-refine over every file in `made`, in place, each through `run` (which
// returns the child's exit code); the first non-zero code is returned.
int meshlab_refine(const MeshJob& job, const std::vector<std::string>& prefix, const std::vector<std::string>& made,
                   const std::function<int(const std::vector<std::string>&)>& run);

}  // namespace gui
