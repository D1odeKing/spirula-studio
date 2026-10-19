#pragma once

// edge_width_px (app/FrameSharpness.h) on the inference layer's device, from
// host RGB or from RGB already there -- what the built-in decoder converts
// into. 2.7 ms a 5K frame uploaded from the host against 27 ms on one core
// (RTX 5070, edge_width_test). One object per run: it holds the device
// buffers until it is destroyed.

#include "nn/vk/Memory.h"

#include <cstdint>
#include <mutex>
#include <string>

namespace blur {

class EdgeWidthGpu {
public:
    EdgeWidthGpu() = default;
    ~EdgeWidthGpu();
    EdgeWidthGpu(const EdgeWidthGpu&) = delete;
    EdgeWidthGpu& operator=(const EdgeWidthGpu&) = delete;

    // Whether the device starts; "" when it does, else why not.
    static std::string availability();

    // The width in `width` x `height` pixels; negative where too few edges were
    // found, as on the host. False when the device failed, which also turns
    // this object off for good. Thread-safe.
    bool measure(const uint8_t* rgb, int width, int height, float& out);
    // `rgb` is width * height * 3 tightly packed bytes on the device.
    bool measure_device(nn::vk::DevicePtr rgb, int width, int height, float& out);

private:
    bool run(nn::vk::DevicePtr rgb, int width, int height, float& out);
    bool grow(nn::vk::DevicePtr& buf, uint64_t& cap, uint64_t bytes, const char* what);

    std::mutex _mu;
    bool _failed = false;
    nn::vk::DevicePtr _rgb = 0, _grey = 0, _stats = 0;
    uint64_t _rgb_cap = 0, _grey_cap = 0;
};

}  // namespace blur
