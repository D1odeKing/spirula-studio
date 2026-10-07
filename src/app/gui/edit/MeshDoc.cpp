// MeshDoc.cpp -- see MeshDoc.h.

#include "app/gui/edit/MeshDoc.h"

#include "i18n/catalog/Edit.h"

#include "mesh/MeshImport.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <stdexcept>

namespace msg = spirula::i18n::msg::edit;

namespace gui {

namespace {

constexpr unsigned char kTint[3] = {255, 108, 13};

std::array<float, 3> centroid_of(const std::array<float, 3>* V,
                                 const std::array<int, 3>& f) {
    std::array<float, 3> c{};
    for (int k = 0; k < 3; k++)
        c[k] = (V[f[0]][k] + V[f[1]][k] + V[f[2]][k]) / 3.0f;
    return c;
}

// The dropped faces in a hash grid of cells one tolerance across, so a
// centroid that came back through decimal text still finds its face.
class CentroidSet {
public:
    CentroidSet(const std::vector<std::array<float, 3>>& pts, float eps)
        : _pts(pts), _eps(std::max(eps, 1e-12f)) {
        for (size_t i = 0; i < pts.size(); i++) _cells.emplace(key(pts[i]), i);
    }
    bool holds(const std::array<float, 3>& q) const {
        const float e2 = _eps * _eps;
        int32_t c[3];
        cell(q, c);
        for (int dz = -1; dz <= 1; dz++)
        for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            const int32_t cc[3] = {c[0] + dx, c[1] + dy, c[2] + dz};
            auto range = _cells.equal_range(pack_cell(cc));
            for (auto it = range.first; it != range.second; ++it) {
                const std::array<float, 3>& p = _pts[it->second];
                float d = 0.0f;
                for (int k = 0; k < 3; k++) {
                    const float t = p[k] - q[k];
                    d += t * t;
                }
                if (d <= e2) return true;
            }
        }
        return false;
    }

private:
    void cell(const std::array<float, 3>& p, int32_t c[3]) const {
        for (int k = 0; k < 3; k++) c[k] = (int32_t)std::floor(p[k] / _eps);
    }
    static uint64_t pack_cell(const int32_t c[3]) {
        uint64_t h = 0xcbf29ce484222325ull;
        for (int k = 0; k < 3; k++)
            h = (h ^ (uint64_t)(uint32_t)c[k]) * 0x100000001b3ull;
        return h;
    }
    uint64_t key(const std::array<float, 3>& p) const {
        int32_t c[3];
        cell(p, c);
        return pack_cell(c);
    }
    const std::vector<std::array<float, 3>>& _pts;
    float _eps;
    std::unordered_multimap<uint64_t, size_t> _cells;
};

}  // namespace

void mesh_drop_faces(const meshing::MeshData& m, const FaceCut& cut,
                     meshing::MeshData& out) {
    out = meshing::MeshData();
    // Same length means the same triangles in the same order, which is what
    // one meshing run writes into every format it was asked for.
    const bool by_index = cut.drop.size() == m.F.size();
    std::unique_ptr<CentroidSet> near_csets;
    if (!by_index && !cut.centroids.empty())
        near_csets = std::make_unique<CentroidSet>(cut.centroids, cut.tolerance);
    std::vector<int> remap(m.V.size(), -1);
    out.F.reserve(m.F.size());
    for (size_t fi = 0; fi < m.F.size(); fi++) {
        const auto& f = m.F[fi];
        const bool drop = by_index
            ? cut.drop[fi] != 0
            : (near_csets && near_csets->holds(centroid_of(m.V.data(), f)));
        if (drop) continue;
        std::array<int, 3> g{};
        for (int k = 0; k < 3; k++) {
            int& r = remap[(size_t)f[k]];
            if (r < 0) {
                r = (int)out.V.size();
                out.V.push_back(m.V[(size_t)f[k]]);
                if (m.N.size() == m.V.size()) out.N.push_back(m.N[(size_t)f[k]]);
                if (m.C.size() == m.V.size()) out.C.push_back(m.C[(size_t)f[k]]);
                if (m.UV.size() == m.V.size()) out.UV.push_back(m.UV[(size_t)f[k]]);
            }
            g[k] = r;
        }
        out.F.push_back(g);
    }
    out.tex_width = m.tex_width;
    out.tex_height = m.tex_height;
    out.texture = m.texture;
}

namespace {

// Rewrite one of them, dropping the faces `dropped` names. Its own colours,
// UVs and texture ride along untouched.
void move_mesh(meshing::MeshData& m, const spirula::Sim3& T) {
    if (T.is_identity()) return;
    for (auto& v : m.V) {
        const double p[3] = {v[0], v[1], v[2]};
        double q[3];
        T.apply(p, q);
        v = {(float)q[0], (float)q[1], (float)q[2]};
    }
    for (auto& nrm : m.N) {
        const double p[3] = {nrm[0], nrm[1], nrm[2]};
        double q[3];
        T.rotate(p, q);
        nrm = {(float)q[0], (float)q[1], (float)q[2]};
    }
}

void filter_sibling(const std::string& path, const FaceCut& cut,
                    const spirula::Sim3& moved) {
    meshing::MeshData m;
    std::string err;
    if (!meshing::read_mesh(path, m, err)) throw std::runtime_error(err);
    meshing::MeshData out;
    // Matched in the coordinates the file was written in, moved after.
    mesh_drop_faces(m, cut, out);
    move_mesh(out, moved);
    meshing::MeshColorMode mode = meshing::MeshColorMode::None;
    if (!out.UV.empty() && !out.texture.empty())
        mode = meshing::MeshColorMode::Texture;
    else if (!out.C.empty())
        mode = meshing::MeshColorMode::Vertex;
    std::string ext = std::filesystem::path(path).extension().string();
    if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    const meshing::MeshFormatSpec spec = meshing::parse_one_mesh_format(ext);
    if (!meshing::check_export_support(spec, mode).empty())
        mode = out.C.empty() ? meshing::MeshColorMode::None
                             : meshing::MeshColorMode::Vertex;
    meshing::write_mesh(out, mode, spec, meshing::mesh_output_strip_ext(path),
                        false);
}

bool textured(const meshing::MeshData& m) {
    return m.UV.size() == m.V.size() && m.tex_width > 0 && m.tex_height > 0 &&
           m.texture.size() >= (size_t)m.tex_width * m.tex_height * 3;
}

std::array<unsigned char, 3> sample_texture(const meshing::MeshData& m,
                                            size_t i) {
    const float u = m.UV[i][0], v = m.UV[i][1];
    const int x = std::clamp((int)(u * (float)m.tex_width), 0, m.tex_width - 1);
    const int y = std::clamp((int)(v * (float)m.tex_height), 0, m.tex_height - 1);
    const size_t o = ((size_t)y * m.tex_width + x) * 3;
    return {m.texture[o], m.texture[o + 1], m.texture[o + 2]};
}

}  // namespace


MeshDoc::MeshDoc(meshing::MeshData mesh, const std::string& source,
                 const float to_view[12],
                 std::function<void(const meshing::MeshData&, const float*)> show)
    : _m(std::move(mesh)), _show(std::move(show)) {
    if (to_view) for (int i = 0; i < 12; i++) _t2n[i] = to_view[i];
    const int64_t n = (int64_t)_m.V.size();
    std::vector<float> pos((size_t)n * 3);
    for (int64_t i = 0; i < n; i++) {
        const auto& p = _m.V[(size_t)i];
        for (int r = 0; r < 3; r++)
            pos[(size_t)i * 3 + r] = _t2n[r*4+0]*p[0] + _t2n[r*4+1]*p[1] +
                                     _t2n[r*4+2]*p[2] + _t2n[r*4+3];
    }
    _live_faces = (int64_t)_m.F.size();
    _edges.reserve(_m.F.size() * 6 + (size_t)n * 2);
    for (const auto& f : _m.F)
        for (int k = 0; k < 3; k++) {
            _edges.push_back(f[k]);
            _edges.push_back(f[(k + 1) % 3]);
        }
    // A texture atlas SPLITS vertices along its seams, so the face graph
    // alone reports one surface as one piece per chart. Seam copies share a
    // position exactly, so welding by position puts the surface back.
    {
        std::vector<int32_t> order((size_t)n);
        for (int64_t i = 0; i < n; i++) order[(size_t)i] = (int32_t)i;
        const auto& V = _m.V;
        std::sort(order.begin(), order.end(), [&V](int32_t a, int32_t b) {
            return V[(size_t)a] < V[(size_t)b];
        });
        for (size_t i = 1; i < order.size(); i++)
            if (V[(size_t)order[i]] == V[(size_t)order[i - 1]]) {
                _edges.push_back(order[i - 1]);
                _edges.push_back(order[i]);
            }
    }

    _display.V = _m.V;
    _display.N = _m.N;
    const bool has_uv = textured(_m);
    _display.C.resize(_m.V.size());
    for (size_t i = 0; i < _m.V.size(); i++) {
        if (has_uv) _display.C[i] = sample_texture(_m, i);
        else if (_m.C.size() == _m.V.size()) _display.C[i] = _m.C[i];
        else _display.C[i] = {184, 184, 184};
    }
    set_source(source);
    add_layer(msg::elem_vertex, n, std::move(pos));
}

int64_t MeshDoc::pick(const ViewProjection& view, float px, float py) const {
    float ro[3], rd[3];
    if (!view.unproject(px, py, ro, rd)) return -1;
    const float* P = positions();
    const uint8_t* live = alive();
    int64_t best = -1;
    float best_t = 1e30f;
    // Moller-Trumbore over the live faces. A click is not a frame, so brute
    // force is the right amount of machinery for it.
    for (const auto& f : _m.F) {
        if (!(live[f[0]] && live[f[1]] && live[f[2]])) continue;
        const float* a = P + (size_t)f[0] * 3;
        const float* b = P + (size_t)f[1] * 3;
        const float* c = P + (size_t)f[2] * 3;
        float e1[3], e2[3], pv[3];
        for (int k = 0; k < 3; k++) {
            e1[k] = b[k] - a[k];
            e2[k] = c[k] - a[k];
        }
        pv[0] = rd[1]*e2[2] - rd[2]*e2[1];
        pv[1] = rd[2]*e2[0] - rd[0]*e2[2];
        pv[2] = rd[0]*e2[1] - rd[1]*e2[0];
        const float det = e1[0]*pv[0] + e1[1]*pv[1] + e1[2]*pv[2];
        if (std::fabs(det) < 1e-20f) continue;
        const float inv = 1.0f / det;
        float tv[3];
        for (int k = 0; k < 3; k++) tv[k] = ro[k] - a[k];
        const float u = (tv[0]*pv[0] + tv[1]*pv[1] + tv[2]*pv[2]) * inv;
        if (u < 0.0f || u > 1.0f) continue;
        float qv[3];
        qv[0] = tv[1]*e1[2] - tv[2]*e1[1];
        qv[1] = tv[2]*e1[0] - tv[0]*e1[2];
        qv[2] = tv[0]*e1[1] - tv[1]*e1[0];
        const float v = (rd[0]*qv[0] + rd[1]*qv[1] + rd[2]*qv[2]) * inv;
        if (v < 0.0f || u + v > 1.0f) continue;
        const float t = (e2[0]*qv[0] + e2[1]*qv[1] + e2[2]*qv[2]) * inv;
        if (t <= 1e-6f || t >= best_t) continue;
        best_t = t;
        best = f[0];
    }
    return best;
}

void MeshDoc::publish_impl(bool geometry) {
    if (!_show) return;
    const uint8_t* alive = this->alive();
    const uint8_t* sel = this->sel().data();
    const bool has_uv = textured(_m);
    for (size_t i = 0; i < _m.V.size(); i++) {
        std::array<unsigned char, 3> base;
        if (has_uv) base = sample_texture(_m, i);
        else if (_m.C.size() == _m.V.size()) base = _m.C[i];
        else base = {184, 184, 184};
        _display.C[i] = sel[i] ? std::array<unsigned char, 3>{kTint[0], kTint[1],
                                                              kTint[2]}
                               : base;
    }
    if (geometry || _display.F.empty()) {
        _display.F.clear();
        _display.F.reserve(_m.F.size());
        for (const auto& f : _m.F)
            if (alive[f[0]] && alive[f[1]] && alive[f[2]]) _display.F.push_back(f);
        _live_faces = (int64_t)_display.F.size();
        // The panes showing the other outputs follow the deletions but not
        // the selection tint: they are the same surface, not the same edit.
        if (_preview_siblings) _preview_siblings(dropped_faces());
    }
    _show(_display, _t2n);
}

FaceCut MeshDoc::dropped_faces() const {
    const uint8_t* live = alive();
    FaceCut cut;
    cut.drop.assign(_m.F.size(), 0);
    for (size_t i = 0; i < _m.F.size(); i++) {
        const auto& f = _m.F[i];
        if (live[f[0]] && live[f[1]] && live[f[2]]) continue;
        cut.drop[i] = 1;
        cut.centroids.push_back(centroid_of(_m.V.data(), f));
    }
    // Loose enough to survive a decimal round trip, tight enough that no two
    // triangles of one surface share a match.
    cut.tolerance = extent() * 1e-4f;
    return cut;
}

void MeshDoc::revert_display() {
    if (_show) _show(_m, _t2n);
}

std::vector<SaveTarget> MeshDoc::save_targets() const {
    return {{&msg::target_mesh_ply, ".ply", false},
            {&msg::target_mesh_obj, ".obj", false},
            {&msg::target_mesh_gltf, ".gltf", false},
            {&msg::target_mesh_glb, ".glb", false},
            {&msg::target_mesh_stl, ".stl", false}};
}

// Case-sensitive, as mesh_output_strip_ext is: anything else and the write
// lands beside the source instead of over it.
std::string MeshDoc::default_save_path(int target) const {
    const std::vector<SaveTarget> t = save_targets();
    if (target < 0 || target >= (int)t.size()) return {};
    const std::string& s = source_path();
    const std::string& ext = t[(size_t)target].ext;
    const bool same = s.size() > ext.size() &&
                      s.compare(s.size() - ext.size(), ext.size(), ext) == 0;
    return same ? s : std::string();
}

void MeshDoc::set_siblings(std::vector<std::string> paths) {
    _siblings.clear();
    for (std::string& p : paths)
        if (p != source_path()) _siblings.push_back(std::move(p));
}



int MeshDoc::save_steps(int) const {
    return 1 + (_link ? (int)_siblings.size() : 0);
}

void MeshDoc::save(int target, const std::string& path,
                   std::atomic<int>* progress) {
    const std::vector<SaveTarget> targets = save_targets();
    const int t = std::clamp(target, 0, (int)targets.size() - 1);
    const std::vector<uint8_t>& keep = alive_of(0);

    // Drop the faces a deleted vertex took with it, then the vertices nothing
    // refers to any more -- a file full of orphans is not what was asked for.
    meshing::MeshData out;
    const uint8_t* alive = keep.data();
    std::vector<int> remap(_m.V.size(), -1);
    out.F.reserve(_m.F.size());
    for (const auto& f : _m.F) {
        if (!(alive[f[0]] && alive[f[1]] && alive[f[2]])) continue;
        std::array<int, 3> g{};
        for (int k = 0; k < 3; k++) {
            int& r = remap[(size_t)f[k]];
            if (r < 0) {
                r = (int)out.V.size();
                out.V.push_back(_m.V[(size_t)f[k]]);
                if (_m.N.size() == _m.V.size()) out.N.push_back(_m.N[(size_t)f[k]]);
                if (_m.C.size() == _m.V.size()) out.C.push_back(_m.C[(size_t)f[k]]);
                if (_m.UV.size() == _m.V.size()) out.UV.push_back(_m.UV[(size_t)f[k]]);
            }
            g[k] = r;
        }
        out.F.push_back(g);
    }
    out.tex_width = _m.tex_width;
    out.tex_height = _m.tex_height;
    out.texture = _m.texture;
    const spirula::Sim3 moved = file_placement();
    move_mesh(out, moved);

    meshing::MeshColorMode mode = meshing::MeshColorMode::None;
    if (!out.UV.empty() && !out.texture.empty())
        mode = meshing::MeshColorMode::Texture;
    else if (!out.C.empty())
        mode = meshing::MeshColorMode::Vertex;
    const meshing::MeshFormatSpec spec =
        meshing::parse_one_mesh_format(targets[(size_t)t].ext);
    if (!meshing::check_export_support(spec, mode).empty()) {
        mode = out.C.empty() ? meshing::MeshColorMode::None
                             : meshing::MeshColorMode::Vertex;
        if (!meshing::check_export_support(spec, mode).empty())
            mode = meshing::MeshColorMode::None;
    }
    meshing::write_mesh(out, mode, spec,
                        meshing::mesh_output_strip_ext(path), false);
    if (progress) (*progress)++;

    // The run's other formats are the same surface, so what left this one
    // leaves them too -- matched by position, since the atlas renumbers.
    if (!_link || _siblings.empty()) return;
    const FaceCut cut = dropped_faces();
    if (cut.empty() && moved.is_identity()) return;
    for (const std::string& s : _siblings) {
        filter_sibling(s, cut, moved);
        if (progress) (*progress)++;
    }
}

bool MeshDoc::colours_available() const {
    return textured(_m) || _m.C.size() == _m.V.size();
}

bool MeshDoc::colours(std::vector<float>& rgb) const {
    const bool has_uv = textured(_m);
    if (!has_uv && _m.C.size() != _m.V.size()) return false;
    rgb.resize(_m.V.size() * 3);
    for (size_t i = 0; i < _m.V.size(); i++) {
        const std::array<unsigned char, 3> c =
            has_uv ? sample_texture(_m, i) : _m.C[i];
        for (int k = 0; k < 3; k++) rgb[i * 3 + k] = c[(size_t)k] / 255.0f;
    }
    return true;
}

bool MeshDoc::normals(std::vector<float>& n, std::vector<float>& w) const {
    if (_m.N.size() != _m.V.size()) return false;
    const size_t num = _m.V.size();
    n.resize(num * 3);
    w.assign(num, 0.0f);
    for (size_t i = 0; i < num; i++)
        for (int r = 0; r < 3; r++) n[i * 3 + r] = _m.N[i][r];
    // A vertex speaks for a third of every face it is on.
    for (const auto& f : _m.F) {
        const auto& a = _m.V[(size_t)f[0]];
        const auto& b = _m.V[(size_t)f[1]];
        const auto& c = _m.V[(size_t)f[2]];
        const float e1[3] = {b[0]-a[0], b[1]-a[1], b[2]-a[2]};
        const float e2[3] = {c[0]-a[0], c[1]-a[1], c[2]-a[2]};
        const float cx = e1[1]*e2[2] - e1[2]*e2[1];
        const float cy = e1[2]*e2[0] - e1[0]*e2[2];
        const float cz = e1[0]*e2[1] - e1[1]*e2[0];
        const float area = 0.5f * std::sqrt(cx*cx + cy*cy + cz*cz) / 3.0f;
        for (int k = 0; k < 3; k++) w[(size_t)f[k]] += area;
    }
    return true;
}

}  // namespace gui
