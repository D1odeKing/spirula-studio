#pragma once

// Closest point on a triangle mesh, and area-uniform samples of its surface:
// what mesh-eval scores with and mesh-refine anchors through. Host only,
// double precision, queries are const and thread-safe.

#include "mesh/MeshExport.h"

#include <cstdint>
#include <vector>

namespace meshing {

class TriangleBvh {
public:
    explicit TriangleBvh(const MeshData& mesh);

    // Squared distance from q to the mesh, the point that attains it and its
    // face; a negative distance when the mesh has no faces.
    double closest(const double q[3], double point[3] = nullptr, int* face = nullptr) const;
    // Unit normal of face f, by its winding.
    void face_normal(int f, double n[3]) const;
    bool empty() const { return tris_.empty(); }

private:
    struct Node { float lo[3], hi[3]; int32_t first, count; };   // count 0 = inner
    std::vector<double> tris_;    // [F, 9]
    std::vector<int32_t> order_;
    std::vector<Node> nodes_;
    int build(int first, int count, std::vector<double>& centroid);
};

// `n` points spread uniformly by area over the faces (deterministic for one
// seed), with the face each came from.
void sample_surface(const MeshData& mesh, int64_t n, uint32_t seed,
                    std::vector<double>& xyz, std::vector<int32_t>* face = nullptr);

// Unit eigenvector of the smallest eigenvalue of a symmetric 3x3 (Jacobi
// sweeps; A is destroyed): a point neighbourhood's normal.
void smallest_eigenvector(double A[9], double out[3]);

// Total surface area.
double mesh_area(const MeshData& mesh);

}  // namespace meshing
