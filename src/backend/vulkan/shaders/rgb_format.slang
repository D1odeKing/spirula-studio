// An image buffer whose channels are stored as float32, uint8 (read as x / 255)
// or float16, read as float through its word view so one kernel binds all
// three. The values are core/PixelFormat.h's.

#ifndef SS_RGB_FORMAT
#define SS_RGB_FORMAT

#include "int8_compat.slang"

static const uint kRgbF32 = 0u;
static const uint kRgbU8 = 1u;
static const uint kRgbF16 = 2u;

[ForceInline]
float rgb_load1(uint* buf, gindex_t i, uint fmt) {
    if (fmt == kRgbU8) return float(u8_load(buf, i)) / 255.0f;
    if (fmt == kRgbF16) {
        uint w = buf[i >> 1];
        return f16tof32((i & 1) != 0 ? (w >> 16) : (w & 0xffffu));
    }
    return asfloat(buf[i]);
}

[ForceInline]
float3 rgb_load3(uint* buf, gindex_t pix, uint fmt) {
    return float3(rgb_load1(buf, 3 * pix, fmt), rgb_load1(buf, 3 * pix + 1, fmt),
                  rgb_load1(buf, 3 * pix + 2, fmt));
}

// float16 without 16-bit storage: an even lane writes its own and the next
// lane's pixel as three whole words. Needs a flat dispatch where every lane
// reaches the call, and an even pixel count allocated (docs/notes/vram-high-res.md).
[ForceInline]
void rgb_store3_f16_pair(uint* buf, uint pix, float3 v, bool active) {
    uint h0 = f32tof16(v.x), h1 = f32tof16(v.y), h2 = f32tof16(v.z);
    uint lane = WaveGetLaneIndex();
    uint o0 = WaveReadLaneAt(h0, lane ^ 1u);
    uint o1 = WaveReadLaneAt(h1, lane ^ 1u);
    uint o2 = WaveReadLaneAt(h2, lane ^ 1u);
    if ((lane & 1u) == 0u && active) {
        uint w = (3u * pix) >> 1;
        buf[w] = h0 | (h1 << 16);
        buf[w + 1] = h2 | (o0 << 16);
        buf[w + 2] = o1 | (o2 << 16);
    }
}

[ForceInline]
void rgb_store3(uint* buf, uint pix, float3 v, uint fmt, bool active) {
    if (fmt == kRgbF16) {
        rgb_store3_f16_pair(buf, pix, v, active);
    } else if (active) {
        buf[3 * pix] = asuint(v.x);
        buf[3 * pix + 1] = asuint(v.y);
        buf[3 * pix + 2] = asuint(v.z);
    }
}

#endif
