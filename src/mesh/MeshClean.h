#pragma once

// The cleanup passes generate_mesh() runs after marching tetrahedra, shared
// with the mesh-lab tools (docs/meshlab.md). Defined in MeshingHost.cpp. Each
// pass edits V / F (and C when present) and leaves N / UV / texture stale.

#include "mesh/MeshExport.h"

namespace meshing {

void merge_vertices(MeshData& mesh, float merge_factor, float max_flip_deg,
                    bool verbose, int num_threads);
// Consistent winding per component, then each component flipped to the
// majority of its faces' current winding.
void orient_mesh(MeshData& mesh);
void dedup_faces(MeshData& mesh, bool verbose);
void remove_floaters(MeshData& mesh, int min_faces, bool verbose);
void fill_holes(MeshData& mesh, float ratio, int max_edges, bool verbose);
void split_nonmanifold_vertices(MeshData& mesh, bool verbose);
void fix_degenerate_faces(MeshData& mesh, float angle_deg, bool verbose);
void improve_mesh_quality(MeshData& mesh, int iters, bool verbose, int num_threads);

}  // namespace meshing
