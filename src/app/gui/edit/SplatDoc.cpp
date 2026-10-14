// SplatDoc.cpp -- see SplatDoc.h.

#include "app/gui/edit/SplatDoc.h"

#include "checkpoint/SplatTransform.h"
#include "engine/Engine.h"
#include "i18n/catalog/Edit.h"

#include <algorithm>
#include <cmath>

namespace msg = spirula::i18n::msg::edit;

namespace gui {

namespace {

// Below this the projection's own alpha cull drops the Gaussian before it
// reaches a tile, so a deleted one costs nothing to leave in place.
constexpr float kDeadOpacity = -30.0f;
// The DC band is (colour - 0.5) / C0, so a colour comes back this way.
constexpr float kSh0 = 0.28209479177387814f;
// Deliberately outside [0,1]: the view-dependent bands are still there, and a
// tint inside the displayable range vanishes under them wherever they are
// strong. Past the clamp, red saturates and blue pins off whatever the SH does.
constexpr float kTint[3] = {3.0f, 0.8f, -1.5f};

TorchTensorView tv(std::vector<float>& v, std::vector<int64_t> shape) {
    return {(uint64_t)(uintptr_t)v.data(), (uint32_t)sizeof(float),
            std::move(shape)};
}

}  // namespace


SplatDoc::SplatDoc(spirula::SplatCloud cloud, const std::string& source,
                   const float to_view[12], int slot, std::mutex* mu)
    : _c(std::move(cloud)), _to_view(spirula::Sim3::from_3x4(to_view)),
      _slot(slot), _mu(mu) {
    const int64_t n = _c.num;
    const float scale = std::sqrt(to_view[0] * to_view[0] +
                                  to_view[4] * to_view[4] +
                                  to_view[8] * to_view[8]);
    std::vector<float> pos((size_t)n * 3);
    std::vector<float> radius((size_t)n);
    for (int64_t i = 0; i < n; i++) {
        const float* m = &_c.means[(size_t)i * 3];
        for (int r = 0; r < 3; r++)
            pos[(size_t)i * 3 + r] = to_view[r * 4 + 0] * m[0] +
                                     to_view[r * 4 + 1] * m[1] +
                                     to_view[r * 4 + 2] * m[2] + to_view[r * 4 + 3];
        const float s = std::max(std::max(_c.scales[(size_t)i * 3],
                                          _c.scales[(size_t)i * 3 + 1]),
                                 _c.scales[(size_t)i * 3 + 2]);
        radius[(size_t)i] = std::exp(std::min(s, 8.0f)) * scale;
    }
    _opacity.resize((size_t)n);
    _dc.resize((size_t)n * 3);
    _solid.resize((size_t)n);
    for (int64_t i = 0; i < n; i++)
        _solid[(size_t)i] = 1.0f / (1.0f + std::exp(-_c.opacities[(size_t)i]));
    set_source(source);
    add_layer(msg::elem_gaussian, n, std::move(pos), std::move(radius));
    // Known only once the layer has measured itself: a Gaussian a twentieth
    // of the scene across is sky or fog, whatever its opacity says.
    const float big = 0.05f * extent();
    const float* rad = radii();
    for (int64_t i = 0; i < n; i++)
        if (rad[i] > big) _solid[(size_t)i] = 0.0f;
}

// The render's own answer to "what is at this pixel" is where transmittance
// crosses one half, so the pick walks the ray the same way: front to back,
// each Gaussian taking its share, with the footprint taken as round.
int64_t SplatDoc::pick(const ViewProjection& view, float px, float py) const {
    struct Hit { float depth, alpha; int64_t index; };
    std::vector<Hit> hits;
    const float* pos = positions();
    const float* rad = radii();
    const uint8_t* live = alive();
    const int64_t n = count();
    for (int64_t i = 0; i < n; i++) {
        if (!live[i]) continue;
        float ux, uy, d;
        if (!view.project(pos + i * 3, ux, uy, d) || d <= 0.0f) continue;
        const float r = std::max(rad[i] * view.fx / d, 0.5f);
        const float dx = ux - px, dy = uy - py, q = (dx * dx + dy * dy) / (r * r);
        if (q > 9.0f) continue;
        const float op = 1.0f / (1.0f + std::exp(-_c.opacities[(size_t)i]));
        const float a = op * std::exp(-0.5f * q);
        if (a > 0.02f) hits.push_back({d, a, i});
    }
    if (hits.empty()) return -1;
    std::sort(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b) { return a.depth < b.depth; });
    float T = 1.0f, best_w = 0.0f;
    int64_t best = hits[0].index;
    for (const Hit& h : hits) {
        const float w = T * h.alpha;
        if (w > best_w) { best_w = w; best = h.index; }
        T *= 1.0f - std::min(h.alpha, 0.99f);
        if (T < 0.5f) return h.index;
    }
    // Never half opaque: a thin spot. What contributed most is what is seen.
    return best;
}

void SplatDoc::publish_impl(bool geometry) {
    if (_slot < 0 || !_mu) return;
    const int64_t n = count();
    const uint8_t* alive = this->alive();
    const uint8_t* sel = this->sel().data();
    for (int64_t i = 0; i < n; i++) {
        _opacity[(size_t)i] = alive[i] ? _c.opacities[(size_t)i] : kDeadOpacity;
        for (int k = 0; k < 3; k++) {
            const float base = _c.features_dc[(size_t)i * 3 + k];
            const float t = (kTint[k] - 0.5f) / kSh0;
            const float w = sel[i] * (1.0f / 255.0f);
            _dc[(size_t)i * 3 + k] = base + w * (t - base);
        }
    }
    std::lock_guard<std::mutex> lk(*_mu);
    if (geometry) engine_scene_update(_slot, "opacities", tv(_opacity, {n, 1}));
    engine_scene_update(_slot, "features_dc", tv(_dc, {n, 3}));
}

void SplatDoc::revert_display() {
    if (_slot < 0 || !_mu) return;
    const int64_t n = count();
    std::lock_guard<std::mutex> lk(*_mu);
    engine_scene_update(_slot, "opacities", tv(_c.opacities, {n, 1}));
    engine_scene_update(_slot, "features_dc", tv(_c.features_dc, {n, 3}));
}

std::vector<SaveTarget> SplatDoc::save_targets() const {
    return {{&msg::target_splat_ply, ".ply", false}};
}

std::string SplatDoc::default_save_path(int) const { return source_path(); }

void SplatDoc::save(int, const std::string& path, std::atomic<int>* progress) {
    // Means, orientation, scale AND the view-dependent colour bands and lobes.
    const spirula::SplatTransform moved(file_placement(), _c.basis());
    spirula::write_splat_ply(_c, path, alive(), &moved);
    if (progress) (*progress)++;
}

// The DC band back to a colour. A linear model goes through the sRGB curve,
// extended past 1 rather than clipped: a highlight at 4.0 is still brighter
// than one at 2.0, and a histogram that cannot tell is no use on an HDR model.
bool SplatDoc::colours(std::vector<float>& rgb) const {
    const int64_t n = _c.num;
    rgb.resize((size_t)n * 3);
    const bool linear = _linear;
#pragma omp parallel for schedule(static)
    for (int64_t i = 0; i < n * 3; i++) {
        float v = 0.5f + kSh0 * _c.features_dc[(size_t)i];
        if (linear) {
            const float a = std::fabs(v);
            const float e = a <= 0.0031308f ? 12.92f * a
                                            : 1.055f * std::pow(a, 1.0f / 2.4f) - 0.055f;
            v = v < 0 ? -e : e;
        }
        rgb[(size_t)i] = v;
    }
    return true;
}

// A Gaussian much thinner one way than the others is a piece of surface, and
// its thin axis is that surface's normal. The rest say nothing.
bool SplatDoc::normals(std::vector<float>& n, std::vector<float>& w) const {
    const int64_t num = _c.num;
    n.assign((size_t)num * 3, 0.0f);
    w.assign((size_t)num, 0.0f);
#pragma omp parallel for schedule(static)
    for (int64_t i = 0; i < num; i++) {
        const float* sc = &_c.scales[(size_t)i * 3];
        int lo = 0;
        if (sc[1] < sc[lo]) lo = 1;
        if (sc[2] < sc[lo]) lo = 2;
        const float a = sc[(lo + 1) % 3], b = sc[(lo + 2) % 3];
        // Log scales: thinner than a third of the smaller in-plane extent.
        if (sc[lo] > std::min(a, b) - 1.1f) continue;
        const float* q = &_c.quats[(size_t)i * 4];
        float qn = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
        if (!(qn > 1e-12f)) continue;
        const float qw = q[0]/qn, x = q[1]/qn, y = q[2]/qn, z = q[3]/qn;
        const float R[9] = {1-2*(y*y+z*z), 2*(x*y-z*qw), 2*(x*z+y*qw),
                            2*(x*y+z*qw), 1-2*(x*x+z*z), 2*(y*z-x*qw),
                            2*(x*z-y*qw), 2*(y*z+x*qw), 1-2*(x*x+y*y)};
        for (int r = 0; r < 3; r++) n[(size_t)i * 3 + r] = R[r * 3 + lo];
        const float op = 1.0f / (1.0f + std::exp(-_c.opacities[(size_t)i]));
        // By linear size rather than area: one huge background splat is
        // one opinion, not a thousand.
        w[(size_t)i] = op * std::exp(0.5f * std::min(a + b, 16.0f));
    }
    return true;
}

}  // namespace gui
