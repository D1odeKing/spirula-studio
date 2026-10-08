// The randomized background draw, shared by the blend kernels
// (pixel_wise_render / pixel_wise_train) and the fused appearance chain.
// Twin of kernels/pixelwise/BackgroundNoise.cuh; the includer brings
// shaders/pixel_wise.slang.

// Port of hash_uint3 in core/Common.cuh (FNV-ish mix + murmur3 finalizer),
// bit-exact.
[ForceInline]
uint _bgn_hash_uint3(uint a, uint b, uint c) {
    uint hash = a;
    hash *= 0x01000193u;
    hash = (hash << 16) | (hash >> 16);
    hash ^= b;
    hash *= 0x01000193u;
    hash = (hash << 16) | (hash >> 16);
    hash ^= c;
    hash *= 0x01000193u;
    hash ^= hash >> 15;
    hash *= 0x85ebca6bu;
    hash ^= hash >> 13;
    hash *= 0xc2b2ae35u;
    hash ^= hash >> 16;
    return hash;
}

// Murmur-style finalizer. The whole background is computed, never stored.
[ForceInline]
uint _bg_mix(uint x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// `blocky` draws a cube corner per cell -- the extremes -- and `block_px` is
// the cell side, 0 being one cell per image.
[ForceInline]
float3 _bg_sample(uint blocky, uint block_px, uint seed, uint gid, uint bid,
                  uint W) {
    uint cx = 0u, cy = 0u, cells_w = 1u;
    if (block_px != 0u) {
        // The cell grid shifts every step, so no pixel keeps its colour and
        // the pattern cannot be baked into the splats.
        cx = ((gid % W) + _bg_mix(seed * 2u + 1u) % block_px) / block_px;
        cy = ((gid / W) + _bg_mix(seed * 2u + 7u) % block_px) / block_px;
        cells_w = W / block_px + 2u;
    }
    if (blocky != 0u) {
        uint h = _bg_mix(cx * 2654435761u ^ cy * 40503u
                         ^ (seed + bid * 0x9e3779b9u));
        return float3((h & 1u) != 0u ? 1.0f : -1.0f,
                      (h & 2u) != 0u ? 1.0f : -1.0f,
                      (h & 4u) != 0u ? 1.0f : -1.0f);
    }
    uint cid = cy * cells_w + cx;
    // 2u-1: without it the plain path lands in [0.5, 0.5+w/2) instead of
    // straddling 0.5, so every channel sits in the same bright half.
    return float3(float(_bgn_hash_uint3(seed + 0, cid, bid)) * exp2(-31.0f) - 1.0f,
                  float(_bgn_hash_uint3(seed + 1, cid, bid)) * exp2(-31.0f) - 1.0f,
                  float(_bgn_hash_uint3(seed + 2, cid, bid)) * exp2(-31.0f) - 1.0f);
}

[ForceInline]
float _bg_exponent(uint bid, uint match_luma, float* exponent_by_cam,
                   int* cam_indices) {
    return match_luma != 0u ? exponent_by_cam[uint(cam_indices[bid])] : 1.0f;
}

// The draw in the working space, gid being the pixel within image bid.
[ForceInline]
float3 _bg_noise_color(uint blocky, uint block_px, uint seed, uint gid,
                       uint bid, uint W, float randomize_weight, float power,
                       int transfer, bool is_linear) {
    float3 background = _bg_sample(blocky, block_px, seed, gid, bid, W);
    background = 0.5f + 0.5f * randomize_weight * background;
    background = background_apply_exponent(background, power);
    return display_to_working3(background, transfer, is_linear);
}
