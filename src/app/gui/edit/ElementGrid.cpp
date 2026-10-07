// ElementGrid.cpp -- see ElementGrid.h.

#include "app/gui/edit/ElementGrid.h"

#include "app/gui/edit/EditDoc.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace gui {

namespace {

// 21 bits per axis, biased to unsigned: a cell index further out than this is
// an outlier that gets clamped, and clamping only ever merges cells that were
// already empty of anything a query would accept.
constexpr int32_t kCoordBias = 1 << 20;
constexpr int32_t kCoordMax = (1 << 21) - 1;
// How far an element's own extent may reach, in cells. Without a cap one
// enormous splat walks the whole grid.
constexpr int kMaxExtentCells = 3;

uint64_t pack(const int32_t c[3]) {
    uint64_t k = 0;
    for (int d = 0; d < 3; d++) {
        const int32_t v = std::clamp(c[d] + kCoordBias, 0, kCoordMax);
        k = (k << 21) | (uint64_t)v;
    }
    return k;
}

uint64_t mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

uint64_t table_size_for(int64_t n) {
    uint64_t m = 16;
    while (m < (uint64_t)std::max<int64_t>(n, 1) * 2) m <<= 1;
    return m;
}

// Union-find, path-halved and always linking the larger index under the
// smaller so the order two merges arrive in cannot matter.
class UnionFind {
public:
    explicit UnionFind(int64_t n) : _p((size_t)n) {
        for (int64_t i = 0; i < n; i++) _p[(size_t)i] = (int32_t)i;
    }
    int32_t find(int32_t x) {
        while (_p[(size_t)x] != x) {
            _p[(size_t)x] = _p[(size_t)_p[(size_t)x]];
            x = _p[(size_t)x];
        }
        return x;
    }
    void unite(int32_t a, int32_t b) {
        a = find(a);
        b = find(b);
        if (a == b) return;
        if (a > b) std::swap(a, b);
        _p[(size_t)b] = a;
    }

private:
    std::vector<int32_t> _p;
};

}  // namespace


void ElementGrid::clear() {
    _pos = nullptr;
    _n = 0;
    _cell = 0.0f;
    _items.clear();
    _beg.clear();
    _of_element.clear();
    _coords.clear();
    _hkey.clear();
    _hval.clear();
    _hmask = 0;
}

void ElementGrid::coords_of(const float* p, int32_t c[3]) const {
    for (int d = 0; d < 3; d++)
        c[d] = (int32_t)std::floor((p[d] - _origin[d]) / _cell);
}

int32_t ElementGrid::find_cell(const int32_t c[3]) const {
    if (_hval.empty()) return -1;
    const uint64_t key = pack(c);
    uint64_t slot = mix(key) & _hmask;
    while (true) {
        const int32_t v = _hval[(size_t)slot];
        if (v < 0) return -1;
        if (_hkey[(size_t)slot] == key) return v;
        slot = (slot + 1) & _hmask;
    }
}

void ElementGrid::build(const EditDoc& doc, float cell) {
    const float* pos = doc.positions();
    const int64_t n = doc.count();
    clear();
    if (n <= 0 || !(cell > 0.0f)) return;
    _pos = pos;
    _n = n;
    _cell = cell;
    const float* m = doc.middle();
    for (int d = 0; d < 3; d++) _origin[d] = m[d] - doc.extent();

    const uint64_t size = table_size_for(n);
    _hmask = size - 1;
    _hkey.assign((size_t)size, 0);
    _hval.assign((size_t)size, -1);

    // One pass: intern each element's cell and count it, in first-seen order.
    std::vector<int32_t> count;
    _of_element.resize((size_t)n);
    count.reserve((size_t)n / 4 + 16);
    _coords.reserve((size_t)n / 4 * 3 + 48);
    for (int64_t i = 0; i < n; i++) {
        int32_t c[3];
        coords_of(pos + i * 3, c);
        const uint64_t key = pack(c);
        uint64_t slot = mix(key) & _hmask;
        while (_hval[(size_t)slot] >= 0 && _hkey[(size_t)slot] != key)
            slot = (slot + 1) & _hmask;
        if (_hval[(size_t)slot] < 0) {
            _hkey[(size_t)slot] = key;
            _hval[(size_t)slot] = (int32_t)count.size();
            count.push_back(0);
            for (int d = 0; d < 3; d++) _coords.push_back(c[d]);
        }
        const int32_t k = _hval[(size_t)slot];
        _of_element[(size_t)i] = k;
        count[(size_t)k]++;
    }

    _beg.assign(count.size() + 1, 0);
    for (size_t k = 0; k < count.size(); k++) _beg[k + 1] = _beg[k] + count[k];
    _items.resize((size_t)n);
    std::vector<int32_t> cursor(_beg.begin(), _beg.end() - 1);
    for (int64_t i = 0; i < n; i++)
        _items[(size_t)cursor[(size_t)_of_element[(size_t)i]]++] = (int32_t)i;
}

// Converges on a cell holding about four elements. Each pass is one O(n)
// intern, so a handful of them costs tens of milliseconds at a million.
float ElementGrid::measure_spacing(const EditDoc& doc) {
    constexpr double kTarget = 4.0;
    const int64_t n = doc.count();
    if (n <= 0) return doc.suggested_radius();
    // Start from a SURFACE estimate rather than the volume one: that is where
    // a scanned scene's elements are, and starting ten times too coarse costs
    // several passes.
    float c = (float)(2.0 * doc.extent() / std::sqrt((double)n));
    c = std::max(c, doc.extent() * 1e-6f);
    for (int pass = 0; pass < 8; pass++) {
        build(doc, c);
        const int64_t used = occupied();
        if (used <= 0) break;
        const double occ = (double)n / (double)used;
        if (occ >= 2.0 && occ <= 8.0) break;
        // Halving the cell divides the occupancy by about four on a surface
        // and eight in a volume, so the exponent sits between the two.
        const double k = std::clamp(std::pow(kTarget / occ, 1.0 / 2.5), 0.25, 4.0);
        const float next = (float)(c * k);
        if (!(next > 0.0f) || next == c) break;
        c = next;
    }
    return _cell;
}


// ---------------------------------------------------------------------------
// Neighbour walks. All of them are over CELLS: a pairwise test costs the
// square of what a cell holds, and the cell size is the reach anyway.
// ---------------------------------------------------------------------------

void ElementGrid::grow(std::vector<uint8_t>& sel, const uint8_t* alive,
                       int64_t n) const {
    if (!built() || n != _n) return;
    const int64_t cells = occupied();
    // The strongest weight any cell holds, then one shell of that outward.
    std::vector<uint8_t> hot((size_t)cells, 0);
    for (int64_t i = 0; i < _n; i++)
        if (sel[(size_t)i])
            hot[(size_t)_of_element[(size_t)i]] =
                std::max(hot[(size_t)_of_element[(size_t)i]], sel[(size_t)i]);
    std::vector<uint8_t> next = hot;
    for (int64_t k = 0; k < cells; k++) {
        if (!hot[(size_t)k]) continue;
        const int32_t* c = &_coords[(size_t)k * 3];
        for (int dz = -1; dz <= 1; dz++)
        for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            const int32_t cc[3] = {c[0] + dx, c[1] + dy, c[2] + dz};
            const int32_t j = find_cell(cc);
            if (j >= 0) next[(size_t)j] = std::max(next[(size_t)j], hot[(size_t)k]);
        }
    }
    for (int64_t i = 0; i < _n; i++) {
        if (sel[(size_t)i] || (alive && !alive[i])) continue;
        sel[(size_t)i] = next[(size_t)_of_element[(size_t)i]];
    }
}

void ElementGrid::shrink(std::vector<uint8_t>& sel, const uint8_t* alive,
                         int64_t n) const {
    if (!built() || n != _n) return;
    const int64_t cells = occupied();
    // A cell is "open" when it holds anything live that is not selected.
    std::vector<uint8_t> open((size_t)cells, 0);
    for (int64_t i = 0; i < _n; i++)
        if (!sel[(size_t)i] && (!alive || alive[i]))
            open[(size_t)_of_element[(size_t)i]] = 1;
    std::vector<uint8_t> edge((size_t)cells, 0);
    for (int64_t k = 0; k < cells; k++) {
        if (!open[(size_t)k]) continue;
        const int32_t* c = &_coords[(size_t)k * 3];
        for (int dz = -1; dz <= 1; dz++)
        for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            const int32_t cc[3] = {c[0] + dx, c[1] + dy, c[2] + dz};
            const int32_t j = find_cell(cc);
            if (j >= 0) edge[(size_t)j] = 1;
        }
    }
    for (int64_t i = 0; i < _n; i++)
        if (sel[(size_t)i] && edge[(size_t)_of_element[(size_t)i]])
            sel[(size_t)i] = 0;
}


bool ElementGrid::components(const uint8_t* alive, int64_t n,
                             std::vector<int32_t>& label,
                             std::vector<int64_t>& sizes, const float* radii,
                             float scale,
                             const std::atomic<bool>* cancel) const {
    label.assign((size_t)n, -1);
    sizes.clear();
    if (!built() || n != _n) return true;
    const int64_t cells = occupied();
    UnionFind uf(cells);

    // Every occupied cell, joined to the occupied cells it touches. Half the
    // neighbourhood: an unordered pair only has to be found once.
    for (int64_t k = 0; k < cells; k++) {
        if (cancel && (k & 0xfff) == 0 && cancel->load()) return false;
        const int32_t* c = &_coords[(size_t)k * 3];
        for (int dz = 0; dz <= 1; dz++)
        for (int dy = (dz == 0 ? 0 : -1); dy <= 1; dy++)
        for (int dx = (dz == 0 && dy == 0 ? 1 : -1); dx <= 1; dx++) {
            const int32_t cc[3] = {c[0] + dx, c[1] + dy, c[2] + dz};
            const int32_t j = find_cell(cc);
            if (j >= 0) uf.unite((int32_t)k, j);
        }
    }

    // An element wider than a cell reaches past its own: a big Gaussian in
    // the sky belongs with the ones it overlaps, which are as far off as it
    // is large.
    if (radii) {
        for (int64_t i = 0; i < _n; i++) {
            if (alive && !alive[i]) continue;
            const int m = std::min((int)std::floor(scale * radii[i] / _cell),
                                   kMaxExtentCells);
            if (m <= 0) continue;
            if (cancel && (i & 0xffff) == 0 && cancel->load()) return false;
            const int32_t home = _of_element[(size_t)i];
            const int32_t* c = &_coords[(size_t)home * 3];
            for (int dz = -m; dz <= m; dz++)
            for (int dy = -m; dy <= m; dy++)
            for (int dx = -m; dx <= m; dx++) {
                const int32_t cc[3] = {c[0] + dx, c[1] + dy, c[2] + dz};
                const int32_t j = find_cell(cc);
                if (j >= 0) uf.unite(home, j);
            }
        }
    }

    std::vector<int32_t> remap((size_t)cells, -1);
    for (int64_t i = 0; i < _n; i++) {
        if (alive && !alive[i]) continue;
        const int32_t root = uf.find(_of_element[(size_t)i]);
        int32_t& l = remap[(size_t)root];
        if (l < 0) {
            l = (int32_t)sizes.size();
            sizes.push_back(0);
        }
        label[(size_t)i] = l;
        sizes[(size_t)l]++;
    }
    return true;
}


void ElementGrid::knn_median(int k, const uint8_t* alive, std::vector<float>& out,
                             const std::atomic<bool>* cancel) const {
    constexpr int kMaxRings = 4;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    out.assign((size_t)_n, nan);
    if (!built() || k < 1) return;
#pragma omp parallel
    {
        std::vector<float> d2;
#pragma omp for schedule(dynamic, 1024)
        for (int64_t i = 0; i < _n; i++) {
            if (!alive[i] || (cancel && cancel->load(std::memory_order_relaxed)))
                continue;
            const float* p = _pos + i * 3;
            int32_t c[3];
            coords_of(p, c);
            d2.clear();
            for (int ring = 1; ring <= kMaxRings; ring++) {
                // Only the new shell: the cells at Chebyshev distance `ring`
                // (and, the first time round, everything inside it).
                for (int dz = -ring; dz <= ring; dz++)
                for (int dy = -ring; dy <= ring; dy++)
                for (int dx = -ring; dx <= ring; dx++) {
                    const int cheb = std::max({std::abs(dx), std::abs(dy), std::abs(dz)});
                    if (ring > 1 && cheb != ring) continue;
                    const int32_t cc[3] = {c[0] + dx, c[1] + dy, c[2] + dz};
                    const int32_t cell = find_cell(cc);
                    if (cell < 0) continue;
                    for (int32_t a = _beg[(size_t)cell]; a < _beg[(size_t)cell + 1]; a++) {
                        const int32_t j = _items[(size_t)a];
                        if (j == i || !alive[j]) continue;
                        const float* q = _pos + (size_t)j * 3;
                        const float x = q[0]-p[0], y = q[1]-p[1], z = q[2]-p[2];
                        d2.push_back(x*x + y*y + z*z);
                    }
                }
                if ((int)d2.size() < k) continue;
                std::nth_element(d2.begin(), d2.begin() + (k - 1), d2.end());
                // Everything within `ring` cells of the point has been seen,
                // so a k-th neighbour nearer than that is the true one.
                const float sure = (float)ring * _cell;
                if (d2[(size_t)k - 1] <= sure * sure) break;
            }
            if (d2.empty()) {
                out[(size_t)i] = (float)kMaxRings * _cell;
                continue;
            }
            const size_t use = std::min(d2.size(), (size_t)k);
            if (d2.size() > use) std::nth_element(d2.begin(), d2.begin() + (use - 1), d2.end());
            std::nth_element(d2.begin(), d2.begin() + use / 2, d2.begin() + use);
            out[(size_t)i] = std::sqrt(d2[use / 2]);
        }
    }
}


void components_from_pairs(const int32_t* pairs, int64_t n_pairs,
                           const uint8_t* alive, int64_t n,
                           std::vector<int32_t>& label,
                           std::vector<int64_t>& sizes) {
    label.assign((size_t)n, -1);
    sizes.clear();
    if (n <= 0) return;
    UnionFind uf(n);
    for (int64_t e = 0; e < n_pairs; e++) {
        const int32_t a = pairs[e * 2], b = pairs[e * 2 + 1];
        if (a < 0 || b < 0 || a >= n || b >= n) continue;
        if (alive && (!alive[a] || !alive[b])) continue;
        uf.unite(a, b);
    }
    std::vector<int32_t> remap((size_t)n, -1);
    for (int64_t i = 0; i < n; i++) {
        if (alive && !alive[i]) continue;
        const int32_t root = uf.find((int32_t)i);
        int32_t& l = remap[(size_t)root];
        if (l < 0) {
            l = (int32_t)sizes.size();
            sizes.push_back(0);
        }
        label[(size_t)i] = l;
        sizes[(size_t)l]++;
    }
}

}  // namespace gui
