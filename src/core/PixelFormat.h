#pragma once

// How an image buffer stores each channel. A ground-truth image keeps the form
// it was decoded in, so a full-resolution float copy never exists; the loss
// kernels read all three. Carried as the element size of the TorchTensorView
// that holds the buffer.

#include "core/Tensor.h"

#include <cstdint>

enum class PixelFormat : uint32_t { F32 = 0, U8 = 1, F16 = 2 };

inline PixelFormat pixel_format(const TorchTensorView& tv) {
    switch (std::get<1>(tv)) {
        case 1:  return PixelFormat::U8;
        case 2:  return PixelFormat::F16;
        default: return PixelFormat::F32;
    }
}

inline uint32_t pixel_format_bytes(PixelFormat f) {
    return f == PixelFormat::U8 ? 1u : f == PixelFormat::F16 ? 2u : 4u;
}

// Bytes for `pixels` pixels of `channels` each. A float16 image is rounded to
// an even pixel count: the Vulkan writers store pixel pairs as whole words.
inline size_t pixel_buffer_bytes(PixelFormat f, int64_t pixels, int64_t channels) {
    if (f == PixelFormat::F16) pixels += pixels & 1;
    return (size_t)pixels * (size_t)channels * pixel_format_bytes(f);
}

#ifdef __CUDACC__
#include <cuda_fp16.h>

// Element i of the buffer; uint8 decodes exactly as uint8_image_to_float_raw.
__device__ __forceinline__ float pixel_load1(const void* p, PixelFormat f, size_t i) {
    if (f == PixelFormat::U8) return (float)((const uint8_t*)p)[i] / 255.0f;
    if (f == PixelFormat::F16) return __half2float(((const __half*)p)[i]);
    return ((const float*)p)[i];
}

__device__ __forceinline__ float3 pixel_load3(const void* p, PixelFormat f, size_t pix) {
    if (f == PixelFormat::F32) return ((const float3*)p)[pix];
    return make_float3(pixel_load1(p, f, 3 * pix), pixel_load1(p, f, 3 * pix + 1),
                       pixel_load1(p, f, 3 * pix + 2));
}
#endif

// An image a kernel reads element by element, in whichever format it is
// stored; a plain float pointer converts, so float callers are unchanged.
struct PixelPtr {
    const void* p = nullptr;
    PixelFormat f = PixelFormat::F32;
    PixelPtr() = default;
    PixelPtr(const float* q) : p(q) {}
    PixelPtr(const void* q, PixelFormat fmt) : p(q), f(fmt) {}
#ifdef __CUDACC__
    __device__ float operator[](size_t i) const { return pixel_load1(p, f, i); }
#endif
};

// The writable counterpart, float32 or float16: `out[i] = v` stores in the
// buffer's format.
struct PixelOut {
    void* p = nullptr;
    PixelFormat f = PixelFormat::F32;
    PixelOut() = default;
    PixelOut(float* q) : p(q) {}
    PixelOut(void* q, PixelFormat fmt) : p(q), f(fmt) {}
#ifdef __CUDACC__
    struct Ref {
        void* p;
        PixelFormat f;
        size_t i;
        __device__ void operator=(float v) const {
            if (f == PixelFormat::F16) ((__half*)p)[i] = __float2half(v);
            else ((float*)p)[i] = v;
        }
    };
    __device__ Ref operator[](size_t i) const { return {p, f, i}; }
#endif
};
