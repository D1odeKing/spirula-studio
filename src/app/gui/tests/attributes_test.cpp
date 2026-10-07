// attributes_test -- app/gui/edit/Attributes.h over shapes whose numbers are
// known by hand: a tetrahedron beside an open square for the mesh attributes,
// a lattice for the neighbour distances, small arrays for the histogram and
// the two-attribute plot, and the colours that plot gives a pair of axes.

#include "app/gui/edit/Attributes.h"
#include "app/gui/edit/EditDoc.h"
#include "app/gui/edit/ElementGrid.h"
#include "i18n/catalog/Edit.h"
#include "mesh/MeshExport.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_failures++;
}

bool close_to(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// The least a document can be: one layer of positions, and a mesh when the
// test has one.
class StubDoc : public gui::EditDoc {
public:
    StubDoc(Kind kind, std::vector<float> pos, const meshing::MeshData* mesh = nullptr)
        : _kind(kind), _mesh(mesh) {
        const int64_t n = (int64_t)pos.size() / 3;
        add_layer(spirula::i18n::msg::edit::elem_vertex, n, std::move(pos));
        if (mesh)
            for (const auto& f : mesh->F)
                for (int k = 0; k < 3; k++) {
                    _edges.push_back(f[k]);
                    _edges.push_back(f[(k + 1) % 3]);
                }
    }
    Kind kind() const override { return _kind; }
    const meshing::MeshData* mesh() const override { return _mesh; }
    const int32_t* topology(int64_t& pairs) const override {
        pairs = (int64_t)_edges.size() / 2;
        return _edges.empty() ? nullptr : _edges.data();
    }
    spirula::Sim3 view_frame() const override { return {}; }
    void revert_display() override {}
    std::vector<gui::SaveTarget> save_targets() const override { return {}; }
    void save(int, const std::string&, std::atomic<int>*) override {}

protected:
    void publish_impl(bool) override {}

private:
    Kind _kind;
    const meshing::MeshData* _mesh;
    std::vector<int32_t> _edges;
};

std::vector<float> flat(const meshing::MeshData& m) {
    std::vector<float> p;
    for (const auto& v : m.V) p.insert(p.end(), v.begin(), v.end());
    return p;
}

std::vector<float> values(const gui::EditDoc& doc, gui::Attr a) {
    std::vector<float> v;
    check(gui::attribute_values(doc, a, v, nullptr) &&
              (int64_t)v.size() == doc.count(),
          std::string("values for ") + gui::attr_info(a).name->get());
    v.resize((size_t)doc.count());
    return v;
}

void test_mesh() {
    // A regular tetrahedron (vertices 0-3), then a unit square of two right
    // triangles far off to the side (4-7), its second face wound BACKWARDS.
    meshing::MeshData m;
    m.V = {{1, 1, 1}, {1, -1, -1}, {-1, 1, -1}, {-1, -1, 1},
           {10, 0, 0}, {11, 0, 0}, {11, 1, 0}, {10, 1, 0}};
    m.F = {{0, 1, 2}, {0, 3, 1}, {0, 2, 3}, {1, 3, 2}, {4, 5, 6}, {4, 7, 6}};
    StubDoc doc(gui::EditDoc::Kind::Mesh, flat(m), &m);

    const auto lo = values(doc, gui::Attr::FaceAngleMin);
    const auto hi = values(doc, gui::Attr::FaceAngleMax);
    check(close_to(lo[0], 60, 1e-3) && close_to(hi[0], 60, 1e-3), "tetrahedron corners are 60");
    check(close_to(lo[5], 45, 1e-3) && close_to(hi[5], 90, 1e-3), "square corners are 45 and 90");

    const auto fold = values(doc, gui::Attr::DihedralMax);
    const double tet = std::acos(-1.0 / 3.0) * 180.0 / 3.14159265358979;
    check(close_to(fold[0], tet, 1e-2), "tetrahedron folds by 109.47");
    check(close_to(fold[4], 0, 1e-3), "a flat diagonal folds by 0 whatever the winding");

    const auto val = values(doc, gui::Attr::Valence);
    check(val[0] == 3 && val[4] == 2 && val[5] == 1, "faces around a vertex");

    const auto bare = values(doc, gui::Attr::EdgeFacesMin);
    const auto busy = values(doc, gui::Attr::EdgeFacesMax);
    check(bare[0] == 2 && busy[0] == 2, "a closed surface has two faces per edge");
    check(bare[4] == 1 && busy[4] == 2, "an open edge counts one");

    const auto piece = values(doc, gui::Attr::PieceSize);
    check(piece[0] == 4 && piece[7] == 4, "piece sizes");

    const auto area = values(doc, gui::Attr::FaceArea);
    check(close_to(area[5], 0.5, 1e-5), "mean face area at a vertex");
    const auto edge = values(doc, gui::Attr::EdgeLength);
    check(close_to(edge[0], std::sqrt(8.0), 1e-4), "mean edge length at a vertex");
}

void test_seam() {
    // The same square with its diagonal split into seam copies (4 = 0, 5 = 2):
    // welded, nothing about it may change.
    meshing::MeshData m;
    m.V = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 0}, {1, 1, 0}};
    m.F = {{0, 1, 2}, {4, 5, 3}};
    StubDoc doc(gui::EditDoc::Kind::Mesh, flat(m), &m);
    const auto val = values(doc, gui::Attr::Valence);
    const auto busy = values(doc, gui::Attr::EdgeFacesMax);
    check(val[0] == 2 && val[4] == 2, "seam copies share their welded valence");
    check(busy[0] == 2 && busy[5] == 2, "a seam is not an open edge");
}

void test_spacing() {
    // A 9 x 9 x 9 lattice, 0.5 apart: an interior point has six neighbours at
    // 0.5 and twelve at 0.5 * sqrt 2.
    const int n = 9;
    const float h = 0.5f;
    std::vector<float> pos;
    for (int z = 0; z < n; z++)
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++) pos.insert(pos.end(), {x * h, y * h, z * h});
    StubDoc doc(gui::EditDoc::Kind::Points, pos);
    const size_t mid = (size_t)((4 * n + 4) * n + 4);

    const auto k4 = values(doc, gui::Attr::Knn4);
    const auto k16 = values(doc, gui::Attr::Knn16);
    check(close_to(k4[mid], h, 1e-5), "median of 4 nearest on a lattice");
    check(close_to(k16[mid], h * std::sqrt(2.0), 1e-5), "median of 16 nearest on a lattice");

    const auto origin_dist = values(doc, gui::Attr::OriginDistance);
    check(close_to(origin_dist[mid], 2.0 * std::sqrt(3.0), 1e-5), "distance to the origin");
}

void test_histogram() {
    gui::AttrInfo whole = gui::attr_info(gui::Attr::Valence);
    const std::vector<float> v = {2, 2, 3, 5, 5, 5};
    const std::vector<uint8_t> alive(v.size(), 1);
    const std::vector<uint8_t> sel = {255, 0, 0, 255, 255, 0};
    gui::AttrHistogram h;
    h.build(v, alive.data(), sel.data(), whole);
    check(h.whole && h.bins == 4, "one bin per whole number");
    check(h.end_label(false) == 2 && h.end_label(true) == 5, "whole axis ends on its values");
    check(h.all[0] == 2 && h.all[1] == 1 && h.all[2] == 0 && h.all[3] == 3, "whole counts");
    check(h.selected[0] == 1 && h.selected[3] == 2, "selected counts");

    // The bin for 3, and nothing else.
    std::vector<uint8_t> out;
    gui::select_by_range(v, h, 0.25, 0.5, false, alive.data(), out);
    int picked = 0;
    for (size_t i = 0; i < v.size(); i++) picked += out[i] ? 1 : 0;
    check(picked == 1 && out[2], "a whole bin selects its value only");
}

void test_periodic() {
    const gui::AttrInfo& hue = gui::attr_info(gui::Attr::Hue);
    const std::vector<float> v = {350, 10, 100, 200, std::nanf("")};
    const std::vector<uint8_t> alive(v.size(), 1);
    gui::AttrHistogram h;
    h.build(v, alive.data(), nullptr, hue);
    check(h.periodic && h.lo == 0 && h.hi == 360, "a hue axis is a circle of 360");

    auto picked = [&](double from, double to, bool outside) {
        std::vector<uint8_t> out;
        gui::select_by_range(v, h, from / 360.0, to / 360.0, outside, alive.data(), out);
        std::string s;
        for (uint8_t o : out) s += o ? '1' : '0';
        return s;
    };
    check(picked(340, 380, false) == "11000", "a range through the seam takes both sides");
    check(picked(-20, 20, false) == "11000", "and is the same range a turn earlier");
    check(picked(340, 380, true) == "00110", "outside is the rest of the circle");
    check(picked(90, 110, false) == "00100", "a range clear of the seam is an ordinary one");
    check(picked(30, 390, false) == "11110", "once round is everything that has a hue");
}

void test_density() {
    const gui::AttrInfo& plain = gui::attr_info(gui::Attr::OriginDistance);
    std::vector<float> x, y;
    std::vector<uint8_t> alive, sel;
    for (int i = 0; i < 1000; i++) {
        x.push_back((float)(i % 10));
        y.push_back((float)(i / 100));
        alive.push_back(i % 7 ? 1 : 0);
        sel.push_back(i % 2 ? 255 : 0);
    }
    gui::AttrHistogram hx, hy;
    hx.build(x, alive.data(), sel.data(), plain);
    hy.build(y, alive.data(), sel.data(), plain);
    gui::AttrDensity d;
    std::vector<int32_t> cell;
    d.build(x, hx, y, hy, alive.data(), sel.data(), 10, 10, &cell);

    uint64_t all = 0, picked = 0, live = 0, live_sel = 0;
    for (uint32_t c : d.all) all += c;
    for (uint32_t c : d.selected) picked += c;
    bool dead_unplaced = true;
    for (size_t i = 0; i < x.size(); i++) {
        live += alive[i] ? 1 : 0;
        live_sel += alive[i] && sel[i] ? 1 : 0;
        if (!alive[i] && cell[i] >= 0) dead_unplaced = false;
    }
    check(all == live && picked == live_sel, "every live element lands in one cell");
    check(dead_unplaced, "the dead have no cell");
    // Row 0 is the BOTTOM: the smallest y, with the smallest x on the left.
    check(cell[1] == 0 || cell[1] == 1, "small x and y sit bottom left");
    check(cell[999] == 99, "large x and y sit top right");
}

void test_pair_colour() {
    struct Rgb { double r, g, b; };
    // An axis over the attribute's natural range, asked for one value on it.
    auto axis = [](gui::Attr a, double lo, double hi) {
        gui::AttrHistogram h;
        h.lo = lo;
        h.hi = hi;
        (void)a;
        return h;
    };
    auto range = [&](gui::Attr a) {
        switch (a) {
            case gui::Attr::Hue: return axis(a, 0.0, 360.0);
            case gui::Attr::ChromaU: case gui::Attr::ChromaV: return axis(a, -0.3, 0.3);
            default: return axis(a, 0.0, 1.0);
        }
    };
    auto pair = [&](gui::Attr a, double va, gui::Attr b, double vb) {
        const gui::AttrHistogram ha = range(a), hb = range(b);
        const unsigned c = gui::attr_pair_colour(gui::attr_info(a), ha, (float)ha.frac_of(va),
                                                 gui::attr_info(b), hb, (float)hb.frac_of(vb));
        return Rgb{(c & 255) / 255.0, ((c >> 8) & 255) / 255.0, ((c >> 16) & 255) / 255.0};
    };
    auto luma = [](const Rgb& c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; };
    using gui::Attr;

    Rgb c = pair(Attr::Hue, 0.0, Attr::Saturation, 1.0);
    check(c.r > 0.9 && c.g < 0.05 && c.b < 0.05, "hue 0 at full saturation is red");
    c = pair(Attr::Hue, 0.0, Attr::Saturation, 0.0);
    check(close_to(c.r, c.g, 0.01) && close_to(c.g, c.b, 0.01), "no saturation is grey at any hue");
    const Rgb flipped = pair(Attr::Saturation, 1.0, Attr::Hue, 0.0);
    check(flipped.r > 0.9 && flipped.g < 0.05, "the axes may come either way round");

    c = pair(Attr::Hue, 240.0, Attr::ChromaU, 0.3);
    check(c.b > c.r + 0.4 && c.b > c.g + 0.4, "a blue hue leaning blue is vivid blue");
    c = pair(Attr::Hue, 60.0, Attr::ChromaU, 0.2);
    check(close_to(c.r, c.g, 0.01) && close_to(c.g, c.b, 0.01),
          "a yellow hue cannot lean blue: grey");
    c = pair(Attr::Hue, 0.0, Attr::ChromaV, -0.2);
    check(close_to(c.r, c.b, 0.01), "nor a red hue lean cyan");

    c = pair(Attr::ChromaU, 0.1, Attr::ChromaV, -0.1);
    check(close_to((c.b - luma(c)) / 1.8556, 0.1, 0.01) &&
              close_to((c.r - luma(c)) / 1.5748, -0.1, 0.01),
          "two colour differences give the colour that has both");
    c = pair(Attr::Hue, 120.0, Attr::Luma, 0.3);
    check(close_to(luma(c), 0.3, 0.01) && c.g > c.r && c.g > c.b, "a hue at a luma");
    c = pair(Attr::Hue, 240.0, Attr::Luma, 0.8);
    check(close_to(luma(c), 0.8, 0.02) && c.b >= c.r, "a bright blue pales rather than clips");
    c = pair(Attr::Saturation, 1.0, Attr::ChromaV, 0.2);
    check(c.r > 0.9 && c.g < 0.1, "saturation leaning red is red");

    // A scene's colour differences are small; the END of the axis is vivid
    // whatever the range is.
    {
        gui::AttrHistogram hh = range(Attr::Hue), hv = axis(Attr::ChromaV, -0.05, 0.05);
        const unsigned u = gui::attr_pair_colour(gui::attr_info(Attr::Hue), hh, 0.0f,
                                                 gui::attr_info(Attr::ChromaV), hv, 1.0f);
        check((u & 255) > 230 && ((u >> 8) & 255) < 40, "a narrow axis still ends vivid");
    }

    // Saturation has no hue of its own to be shown in: red stands in.
    {
        const gui::AttrHistogram hsat = axis(Attr::Saturation, 0.0, 1.0);
        const gui::AttrHistogram hsize = axis(Attr::ScaleMean, 0.0, 1.0);
        const unsigned vivid = gui::attr_pair_colour(gui::attr_info(Attr::Saturation), hsat, 1.0f,
                                                     gui::attr_info(Attr::ScaleMean), hsize, 0.5f);
        const unsigned none = gui::attr_pair_colour(gui::attr_info(Attr::Saturation), hsat, 0.0f,
                                                    gui::attr_info(Attr::ScaleMean), hsize, 0.5f);
        check((vivid & 255) > 200 && ((vivid >> 8) & 255) < 30, "full saturation alone is red");
        check((none & 255) == ((none >> 8) & 255), "none is grey");
    }
    c = pair(Attr::Saturation, 1.0, Attr::Luma, 0.6);
    check(c.r > 0.5 && c.g < 0.05, "saturation at a luma is that much red");

    const gui::AttrInfo& hue = gui::attr_info(Attr::Hue);
    const gui::AttrInfo& size = gui::attr_info(Attr::ScaleMean);
    const gui::AttrHistogram hh = range(Attr::Hue), hs = axis(Attr::ScaleMean, 0.0, 1.0);
    check(gui::attr_pair_colour(hue, hh, 0.55f, size, hs, 0.5f) ==
              gui::attr_tint_colour(hue.tint, hh.value_at(0.55), 0.55f),
          "one colour axis colours the cell by itself");
    check(gui::attr_pair_colour(gui::attr_info(Attr::PosX), hs, 0.5f,
                                gui::attr_info(Attr::PosY), hs, 0.5f) == 0,
          "two position axes leave the discs neutral");
}

}  // namespace

int main() {
    test_pair_colour();
    test_mesh();
    test_seam();
    test_spacing();
    test_histogram();
    test_periodic();
    test_density();
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
