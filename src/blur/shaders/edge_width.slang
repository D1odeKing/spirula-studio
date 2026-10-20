// The edge-width blur measure, one frame at a time: the device copy of
// app/FrameSharpness.cpp's edge_width_px. Every constant, the 5-tap smoothing
// and the walk must stay what that file does; blur/EdgeWidth.h holds the
// numbers both read, and the host turns the width histograms into the answer.

#include "_common.slang"

static const int WG = 256;

static const uint  kMagBins = 4096u;      // blur::kMagBins
static const float kMagPerLevel = 4.0f;   // blur::kMagPerLevel
static const int   kReach = 60;           // blur::kReach
static const float kStep = 12.0f;         // blur::kStep
static const uint  kWidthBins = 121u;     // blur::kWidthBins

// Layout of the statistics buffer, in words: two magnitude histograms, the
// two thresholds (f32), then two width histograms. x before y throughout.
static const uint kStrongAt = 2u * kMagBins;
static const uint kWidthAt = kStrongAt + 2u;

uint load_byte(uint* buf, uint i) {
    return (buf[i >> 2u] >> ((i & 3u) * 8u)) & 0xFFu;
}

// ---------------------------------------------------------------------------
// Half-size BT.601 luma, then the separable smoothing
// ---------------------------------------------------------------------------

struct GreyParams {
    uint* rgb;      // src_w * (2 * h) * 3 bytes, packed
    uint* out;      // w * h f32
    uint  src_w;
    uint  w;
    uint  h;
    uint  groups_per_row;
};

[shader("compute")]
[numthreads(WG, 1, 1)]
void half_grey(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uniform GreyParams p) {
    uint i = fold_index(gid, tid, p.groups_per_row, uint(WG));
    if (i >= p.w * p.h) return;
    uint x = i % p.w;
    uint y = i / p.w;
    uint a = ((2u * y) * p.src_w + 2u * x) * 3u;
    uint b = a + p.src_w * 3u;
    float r = float(load_byte(p.rgb, a) + load_byte(p.rgb, a + 3u) + load_byte(p.rgb, b) +
                    load_byte(p.rgb, b + 3u));
    float g = float(load_byte(p.rgb, a + 1u) + load_byte(p.rgb, a + 4u) +
                    load_byte(p.rgb, b + 1u) + load_byte(p.rgb, b + 4u));
    float bl = float(load_byte(p.rgb, a + 2u) + load_byte(p.rgb, a + 5u) +
                     load_byte(p.rgb, b + 2u) + load_byte(p.rgb, b + 5u));
    p.out[i] = asuint(0.25f * (0.299f * r + 0.587f * g + 0.114f * bl));
}

struct SmoothParams {
    uint* src;
    uint* out;
    uint  w;
    uint  h;
    uint  along_x;   // 1: along rows, 0: down columns
    uint  groups_per_row;
};

// Gaussian of sigma 0.8, 5 taps. The two-pixel rim is copied as it came.
[shader("compute")]
[numthreads(WG, 1, 1)]
void smooth(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uniform SmoothParams p) {
    uint i = fold_index(gid, tid, p.groups_per_row, uint(WG));
    if (i >= p.w * p.h) return;
    uint pos = p.along_x != 0u ? i % p.w : i / p.w;
    uint len = p.along_x != 0u ? p.w : p.h;
    uint s = p.along_x != 0u ? 1u : p.w;
    float v = asfloat(p.src[i]);
    if (pos >= 2u && pos + 2u < len) {
        float n1 = asfloat(p.src[i - s]) + asfloat(p.src[i + s]);
        float n2 = asfloat(p.src[i - 2u * s]) + asfloat(p.src[i + 2u * s]);
        v = 0.4992f * v + 0.2285f * n1 + 0.0219f * n2;
    }
    p.out[i] = asuint(v);
}

// ---------------------------------------------------------------------------
// Gradients
// ---------------------------------------------------------------------------

float px_at(uint* g, int i) { return asfloat(g[uint(i)]); }

// Sobel at (x, y) of a `w`-wide map; the caller keeps one pixel clear of the rim.
float2 sobel(uint* g, int x, int y, int w) {
    int c = y * w + x;
    float gx = (px_at(g, c + 1 - w) + 2.0f * px_at(g, c + 1) + px_at(g, c + 1 + w)) -
               (px_at(g, c - 1 - w) + 2.0f * px_at(g, c - 1) + px_at(g, c - 1 + w));
    float gy = (px_at(g, c + w - 1) + 2.0f * px_at(g, c + w) + px_at(g, c + w + 1)) -
               (px_at(g, c - w - 1) + 2.0f * px_at(g, c - w) + px_at(g, c - w + 1));
    return float2(gx, gy);
}

uint mag_bin(float a) {
    return min(uint(a * kMagPerLevel), kMagBins - 1u);
}

struct StatParams {
    uint* grey;    // w * h f32, smoothed
    uint* stats;   // see kStrongAt / kWidthAt
    uint  w;
    uint  h;
    uint  nx;      // sampled columns
    uint  ny;      // sampled rows
    uint  at;      // the rank of the 99th percentile among nx * ny samples
    uint  groups_per_row;
};

// Every fourth pixel each way, from (1, 1): its |gx| and |gy| into the two
// magnitude histograms.
[shader("compute")]
[numthreads(WG, 1, 1)]
void magnitudes(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uniform StatParams p) {
    uint i = fold_index(gid, tid, p.groups_per_row, uint(WG));
    if (i >= p.nx * p.ny) return;
    int x = 1 + 4 * int(i % p.nx);
    int y = 1 + 4 * int(i / p.nx);
    float2 d = sobel(p.grey, x, y, int(p.w));
    uint old;
    InterlockedAdd(p.stats[mag_bin(abs(d.x))], 1u, old);
    InterlockedAdd(p.stats[kMagBins + mag_bin(abs(d.y))], 1u, old);
}

groupshared uint g_count[WG];

// One workgroup: each axis's threshold, the lower edge of the bin holding rank
// `at` (blur::strong_from_hist).
[shader("compute")]
[numthreads(WG, 1, 1)]
void thresholds(uint3 tid : SV_GroupThreadID, uniform StatParams p) {
    uint t = tid.x;
    const uint per = kMagBins / uint(WG);
    for (uint axis = 0u; axis < 2u; ++axis) {
        uint base = axis * kMagBins + t * per;
        uint sum = 0u;
        for (uint k = 0u; k < per; ++k) sum += p.stats[base + k];
        g_count[t] = sum;
        GroupMemoryBarrierWithGroupSync();
        // Inclusive prefix sum over the 256 partial counts.
        for (uint s = 1u; s < uint(WG); s <<= 1u) {
            uint add = t >= s ? g_count[t - s] : 0u;
            GroupMemoryBarrierWithGroupSync();
            g_count[t] += add;
            GroupMemoryBarrierWithGroupSync();
        }
        uint before = t > 0u ? g_count[t - 1u] : 0u;
        if (before <= p.at && g_count[t] > p.at) {
            uint c = before;
            for (uint k = 0u; k < per; ++k) {
                c += p.stats[base + k];
                if (c > p.at) {
                    p.stats[kStrongAt + axis] = asuint(float(t * per + k) / kMagPerLevel);
                    break;
                }
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

// How far a step runs each way from `c` before it stops rising (`up`) or
// falling, along stride `s` with `pos` of `len` places behind it.
uint walk(uint* g, int c, int s, int pos, int len, bool up) {
    int lo = 0;
    while (pos - lo > 0 && lo < kReach) {
        float a = px_at(g, c - (lo + 1) * s), b = px_at(g, c - lo * s);
        if (up ? !(a < b) : !(a > b)) break;
        lo++;
    }
    int hi = 0;
    while (pos + hi < len - 1 && hi < kReach) {
        float a = px_at(g, c + (hi + 1) * s), b = px_at(g, c + hi * s);
        if (up ? !(a > b) : !(a < b)) break;
        hi++;
    }
    if (!(abs(px_at(g, c + hi * s) - px_at(g, c - lo * s)) > kStep)) return kWidthBins;
    return uint(lo + hi);
}

// Every other pixel each way, from (3, 3): an edge across x or across y walked
// to its width. A pixel cannot be both, since each needs twice the other.
[shader("compute")]
[numthreads(WG, 1, 1)]
void widths(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uniform StatParams p) {
    uint i = fold_index(gid, tid, p.groups_per_row, uint(WG));
    if (i >= p.nx * p.ny) return;
    int x = 3 + 2 * int(i % p.nx);
    int y = 3 + 2 * int(i / p.nx);
    int w = int(p.w);
    float2 d = sobel(p.grey, x, y, w);
    float ax = abs(d.x), ay = abs(d.y);
    uint axis = 2u, width = kWidthBins;
    if (ax > asfloat(p.stats[kStrongAt]) && ax > 2.0f * ay) {
        axis = 0u;
        width = walk(p.grey, y * w + x, 1, x, w, d.x > 0.0f);
    } else if (ay > asfloat(p.stats[kStrongAt + 1u]) && ay > 2.0f * ax) {
        axis = 1u;
        width = walk(p.grey, y * w + x, w, y, int(p.h), d.y > 0.0f);
    }
    if (axis < 2u && width < kWidthBins) {
        uint old;
        InterlockedAdd(p.stats[kWidthAt + axis * kWidthBins + width], 1u, old);
    }
}
