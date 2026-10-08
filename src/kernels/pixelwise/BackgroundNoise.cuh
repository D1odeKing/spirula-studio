#pragma once

// The randomized background draw, shared by the blend kernels
// (ImageColorOps.cu) and the fused appearance chain (AppearanceChain.cu).
// Twin of backend/vulkan/shaders/background_noise.slang.

#include "kernels/pixelwise/PixelWiseCommon.cuh"

// Murmur-style finalizer. Cheap, well-distributed, and stateless -- the whole
// background is computed, never stored.
__device__ __forceinline__ uint32_t _bg_mix(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// Unit sample for the background, in [-1, 1] either way. `blocky` draws one of
// the 8 RGB cube corners per cell -- the extremes, so residual transparency
// costs most; `block_px` is the cell side, 0 being one cell per image.
__device__ __forceinline__ float3 _bg_sample(bool blocky, unsigned block_px,
                                             uint32_t seed, unsigned bid,
                                             unsigned x, unsigned y, unsigned W) {
    unsigned cx = 0u, cy = 0u, cells_w = 1u;
    if (block_px) {
        // The whole cell grid shifts each step, so no pixel keeps its colour
        // and the pattern cannot be baked into the splats.
        cx = (x + _bg_mix(seed * 2u + 1u) % block_px) / block_px;
        cy = (y + _bg_mix(seed * 2u + 7u) % block_px) / block_px;
        cells_w = W / block_px + 2u;
    }
    float3 u;
    if (blocky) {
        const uint32_t h = _bg_mix(cx * 2654435761u ^ cy * 40503u
                                   ^ (seed + bid * 0x9e3779b9u));
        u.x = (h & 1u) ? 1.0f : -1.0f;
        u.y = (h & 2u) ? 1.0f : -1.0f;
        u.z = (h & 4u) ? 1.0f : -1.0f;
    } else {
        const unsigned cid = cy * cells_w + cx;
        // 2u-1: without it the plain path lands in [0.5, 0.5+w/2) instead of
        // straddling 0.5, so every channel sits in the same bright half.
        u.x = (float)hash_uint3(seed + 0, cid, bid) * exp2f(-31.0f) - 1.0f;
        u.y = (float)hash_uint3(seed + 1, cid, bid) * exp2f(-31.0f) - 1.0f;
        u.z = (float)hash_uint3(seed + 2, cid, bid) * exp2f(-31.0f) - 1.0f;
    }
    return u;
}

// Per-image power on the display draw; 1 (identity) without a table.
__device__ __forceinline__ float _bg_exponent(unsigned bid,
                                              const float* exponent_by_cam,
                                              const int32_t* cam_indices) {
    return exponent_by_cam ? exponent_by_cam[cam_indices[bid]] : 1.0f;
}

__device__ __forceinline__ float3 _bg_color(int transfer, bool is_linear,
                                            bool blocky, unsigned block_px,
                                            uint32_t seed, unsigned bid,
                                            unsigned x, unsigned y, unsigned W,
                                            float randomize_weight, float p) {
    float3 background = _bg_sample(blocky, block_px, seed, bid, x, y, W);
    background = 0.5 + 0.5*randomize_weight * background;
    background = SlangPixelWise::background_apply_exponent(background, p);
    return SlangPixelWise::display_to_working3(background, transfer, is_linear);
}
