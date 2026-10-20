// edge_width -- the blur measure on the device against the host walk it copies
// (app/FrameSharpness.h, edge_width_px). The two do the same arithmetic in a
// different order, so they agree to a tolerance, not bit for bit.
//
//   ./build_vulkan/edge_width_test                  # synthetic frames, or SKIP
//   ./build_vulkan/edge_width_test a.jpg b.png ...  # and these, with timings

#include "app/FrameSharpness.h"
#include "blur/EdgeWidthGpu.h"

#include "nn/core/Log.h"
#include "nn/io/Image.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "ok  " : "BAD ", what.c_str());
    if (!ok) g_failures++;
}

// A chequerboard of unequal squares smeared across `smear` pixels along x (or
// y), with sensor-like noise of `noise` levels.
std::vector<uint8_t> squares(int w, int h, int smear, bool along_y, int noise, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> n(0.0f, (float)noise);
    std::vector<uint8_t> px((size_t)w * h * 3);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int sum = 0;
            for (int k = 0; k < smear; k++) {
                const int cx = (along_y ? x : x + k) / 37, cy = (along_y ? y + k : y) / 41;
                sum += (cx + cy) % 2 ? 110 + 15 * ((cx + 3 * cy) % 8) : 30;
            }
            for (int c = 0; c < 3; c++) {
                const float v = (float)(sum / smear) + (noise ? n(rng) : 0.0f) + 7.0f * c;
                px[((size_t)y * w + x) * 3 + c] = (uint8_t)std::fmin(255.0f, std::fmax(0.0f, v));
            }
        }
    return px;
}

bool close(float gpu, float cpu) {
    if (cpu < 0.0f || gpu < 0.0f) return cpu < 0.0f && gpu < 0.0f;
    return std::fabs(gpu - cpu) <= 0.02f * cpu + 0.1f;
}

std::string pair(float gpu, float cpu) {
    char b[64];
    std::snprintf(b, sizeof b, " (gpu %.3f, cpu %.3f)", gpu, cpu);
    return b;
}

void compare(blur::EdgeWidthGpu& gpu, const uint8_t* rgb, int w, int h, const std::string& what,
             bool timed) {
    const double t0 = nn::now_ms();
    const float cpu = app::edge_width_px(rgb, w, h);
    const double t1 = nn::now_ms();
    float dev = -2.0f;
    const bool ran = gpu.measure(rgb, w, h, dev);
    const double t2 = nn::now_ms();
    std::string got = pair(dev, cpu);
    if (timed) {
        char b[64];
        std::snprintf(b, sizeof b, " [cpu %.1f ms, gpu %.1f ms]", t1 - t0, t2 - t1);
        got += b;
    }
    expect(ran && close(dev, cpu), what + got);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string why = blur::EdgeWidthGpu::availability();
    if (!why.empty()) {
        std::printf("SKIP  no device: %s\n", why.c_str());
        return 0;
    }
    blur::EdgeWidthGpu gpu;
    for (int smear : {1, 6, 30})
        for (bool along_y : {false, true}) {
            const int w = 640 + smear, h = 480 - smear;   // odd sizes too
            compare(gpu, squares(w, h, smear, along_y, 0, 1).data(), w, h,
                    "smear " + std::to_string(smear) + (along_y ? " down" : " across") + ", " +
                        std::to_string(w) + "x" + std::to_string(h),
                    false);
        }
    compare(gpu, squares(1280, 720, 4, false, 6, 2).data(), 1280, 720, "noisy, 1280x720", false);

    const std::vector<uint8_t> flat((size_t)640 * 480 * 3, 128);
    float none = 0.0f;
    expect(gpu.measure(flat.data(), 640, 480, none) && none < 0.0f, "no edges: not measured");
    expect(gpu.measure(flat.data(), 100, 100, none) && none < 0.0f, "under 64 px: not measured");

    // A 5K frame, and the same twice: a reused object must not carry state.
    const std::vector<uint8_t> big = squares(5120, 2880, 3, true, 4, 3);
    compare(gpu, big.data(), 5120, 2880, "5120x2880", true);
    compare(gpu, big.data(), 5120, 2880, "5120x2880 again", true);

    for (int i = 1; i < argc; i++) {
        const nn::Image im = nn::load_image(argv[i]);
        if (im.empty()) {
            expect(false, std::string("cannot read ") + argv[i]);
            continue;
        }
        compare(gpu, im.data.data(), im.width, im.height, argv[i], true);
    }
    std::printf("%s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures;
}
