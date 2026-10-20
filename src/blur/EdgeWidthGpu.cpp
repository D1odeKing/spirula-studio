// EdgeWidthGpu.cpp -- see EdgeWidthGpu.h.

#include "blur/EdgeWidthGpu.h"

#include "blur/EdgeWidth.h"
#include "nn/vk/Context.h"
#include "nn/vk/EmbeddedSpirv.h"
#include "nn/vk/Stream.h"

#include <algorithm>
#include <exception>
#include <vector>

// Registered from the entry points, not a static initializer: ss_blur is a
// static archive (nn/vk/EmbeddedSpirv.h).
NN_DECLARE_EMBEDDED_MODULES(blur)

namespace blur {

namespace vk = nn::vk;

namespace {

// Must match the uniform structs in shaders/edge_width.slang.
struct GreyParams {
    vk::DevicePtr rgb, out;
    uint32_t src_w, w, h, groups_per_row;
};
struct SmoothParams {
    vk::DevicePtr src, out;
    uint32_t w, h, along_x, groups_per_row;
};
struct StatParams {
    vk::DevicePtr grey, stats;
    uint32_t w, h, nx, ny, at, groups_per_row;
};

constexpr uint32_t kStrongAt = 2 * kMagBins;
constexpr uint32_t kWidthAt = kStrongAt + 2;
constexpr uint32_t kStatWords = kWidthAt + 2 * kWidthBins;

}  // namespace

std::string EdgeWidthGpu::availability() {
    NN_ENSURE_EMBEDDED_MODULES(blur);
    try {
        vk::Context::get();
        return "";
    } catch (const std::exception& e) {
        return e.what();
    }
}

EdgeWidthGpu::~EdgeWidthGpu() {
    if (!_rgb && !_grey && !_stats) return;
    try {
        vk::Stream::get().sync();
        for (vk::DevicePtr p : {_rgb, _grey, _stats})
            if (p) vk::device_free(p);
    } catch (...) {}
}

bool EdgeWidthGpu::grow(vk::DevicePtr& buf, uint64_t& cap, uint64_t bytes, const char* what) {
    if (buf && cap >= bytes) return true;
    if (buf) vk::device_free(buf);
    buf = vk::device_alloc(bytes, what);
    cap = buf ? bytes : 0;
    return buf != 0;
}

bool EdgeWidthGpu::measure(const uint8_t* rgb, int width, int height, float& out) {
    std::lock_guard<std::mutex> lk(_mu);
    if (_failed) return false;
    out = -1.0f;
    if (!rgb || width / 2 < 64 || height / 2 < 64) return true;
    try {
        NN_ENSURE_EMBEDDED_MODULES(blur);
        const uint64_t bytes = (uint64_t)width * height * 3;
        // Whole words, since the kernel reads them; the tail of the last is unused.
        if (grow(_rgb, _rgb_cap, (bytes + 3) & ~3ull, "blur.rgb")) {
            vk::Stream::get().upload(_rgb, rgb, bytes);
            if (run(_rgb, width, height, out)) return true;
        }
    } catch (const std::exception&) {}
    _failed = true;
    return false;
}

bool EdgeWidthGpu::measure_device(vk::DevicePtr rgb, int width, int height, float& out) {
    std::lock_guard<std::mutex> lk(_mu);
    if (_failed) return false;
    out = -1.0f;
    if (!rgb || width / 2 < 64 || height / 2 < 64) return true;
    try {
        NN_ENSURE_EMBEDDED_MODULES(blur);
        if (run(rgb, width, height, out)) return true;
    } catch (const std::exception&) {}
    _failed = true;
    return false;
}

bool EdgeWidthGpu::run(vk::DevicePtr rgb, int width, int height, float& out) {
    vk::Stream& s = vk::Stream::get();
    const uint32_t w = (uint32_t)width / 2, h = (uint32_t)height / 2;
    const uint64_t grey = (uint64_t)w * h * 4;
    if (!grow(_grey, _grey_cap, 2 * grey, "blur.grey")) return false;
    if (!_stats) _stats = vk::device_alloc((uint64_t)kStatWords * 4, "blur.stats");
    s.zero(_stats, (VkDeviceSize)kStatWords * 4);
    const vk::DevicePtr a = _grey, b = _grey + grey;

    GreyParams gp{rgb, a, (uint32_t)width, w, h, 0};
    s.dispatchFlat("edge_width.half_grey", {}, (int64_t)w * h, 256, &gp, sizeof(gp),
                   &gp.groups_per_row);
    SmoothParams sx{a, b, w, h, 1, 0};
    s.dispatchFlat("edge_width.smooth", {}, (int64_t)w * h, 256, &sx, sizeof(sx),
                   &sx.groups_per_row);
    SmoothParams sy{b, a, w, h, 0, 0};
    s.dispatchFlat("edge_width.smooth", {}, (int64_t)w * h, 256, &sy, sizeof(sy),
                   &sy.groups_per_row);

    // The host's two loops: every fourth pixel from 1, every other from 3.
    const uint32_t mx = (w + 1) / 4, my = (h + 1) / 4;
    const uint64_t samples = (uint64_t)mx * my;
    if (samples < 100) return true;
    StatParams mp{a, _stats, w, h, mx, my, (uint32_t)(samples * 99 / 100), 0};
    s.dispatchFlat("edge_width.magnitudes", {}, (int64_t)samples, 256, &mp, sizeof(mp),
                   &mp.groups_per_row);
    s.dispatch("edge_width.thresholds", {}, 1, 1, 1, &mp, sizeof(mp));
    StatParams wp = mp;
    wp.nx = (w - 5) / 2;
    wp.ny = (h - 5) / 2;
    s.dispatchFlat("edge_width.widths", {}, (int64_t)wp.nx * wp.ny, 256, &wp, sizeof(wp),
                   &wp.groups_per_row);

    std::vector<uint32_t> hist(2 * kWidthBins);
    s.download(hist.data(), _stats + (vk::DevicePtr)kWidthAt * 4, hist.size() * 4);
    const float x = width_from_hist(hist.data()), y = width_from_hist(hist.data() + kWidthBins);
    out = x < 0.0f || y < 0.0f ? -1.0f : 2.0f * std::max(x, y);
    return true;
}

}  // namespace blur
