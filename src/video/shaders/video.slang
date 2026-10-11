// Decoded-video post-processing: YCbCr to RGB, and the sharpness metric that
// picks the least motion-blurred frame of a window.
//
// The decoder hands back multi-planar images; their planes are copied into
// plain buffers (one vkCmdCopyImageToBuffer per aspect) and read here through
// the same `uint*` convention as every other kernel -- 8-bit samples come out
// of a word four at a time, 10/12/16-bit two at a time, so no 8- or 16-bit
// storage feature is needed.

#include "_common.slang"

static const int WG = 256;

// flags bits, shared by every entry point here.
static const uint kF_TwoBytes = 1u << 0;   // 16-bit plane samples
static const uint kF_SubX     = 1u << 1;   // chroma is half-width
static const uint kF_SubY     = 1u << 2;   // chroma is half-height
static const uint kF_Planar   = 1u << 3;   // Cb and Cr in separate planes
static const uint kF_RotShift = 4u;        // 2 bits: 0/90/180/270 clockwise
static const uint kF_ShiftAt  = 8u;        // yuv_to_rgb: 8 bits of sample shift
static const uint kF_Lut      = 1u << 16;  // yuv_to_rgb: map through p.lut

// One luma/chroma sample. `stride` and `idx` are in texels, not bytes.
float load_sample(uint* buf, uint idx, uint flags, uint shift) {
    if ((flags & kF_TwoBytes) != 0u) {
        uint w = buf[idx >> 1u];
        uint v = (w >> ((idx & 1u) * 16u)) & 0xFFFFu;
        return float(v >> shift);
    }
    uint w = buf[idx >> 2u];
    return float((w >> ((idx & 3u) * 8u)) & 0xFFu);
}

// ---------------------------------------------------------------------------
// YCbCr -> RGB, with box downscale and rotation folded in
// ---------------------------------------------------------------------------

struct YuvParams {
    // Rows of the 3x4 conversion matrix, in [Y, Cb, Cr, 1] order. The host
    // bakes bit depth, studio/full range and the BT.601/709/2020 coefficients
    // into it, so the shader has one code path for every combination.
    float4 m0;
    float4 m1;
    float4 m2;
    uint*  out;      // ceil(out_w*out_h*3 / 4) words of packed RGB bytes
    uint*  luma;
    uint*  cb;
    uint*  cr;       // == cb for interleaved (2-plane) formats
    uint   out_w;
    uint   out_h;
    uint   src_w;
    uint   src_h;
    uint   luma_stride;    // texels per row
    uint   chroma_stride;  // texels per row (2 components per texel if !planar)
    uint   flags;          // with the sample shift at kF_ShiftAt: 128 bytes is the limit
    float  inv_sx;         // source texels per scaled texel
    float  inv_sy;
    uint   groups_per_row;
    float* lut;            // size, min[3], (size-1)/(max-min)[3], pad; then size^3 RGB
};

// A 3D LUT, tetrahedrally, as app::apply_cube_lut does it on the host. `c` is
// in [0, 1]; red varies fastest in the table.
float3 map_lut(float* L, float3 c) {
    uint n = uint(L[0]);
    float3 x = clamp((c - float3(L[1], L[2], L[3])) * float3(L[4], L[5], L[6]),
                     float3(0.0f), float3(float(n - 1u)));
    uint3 i = min(uint3(x), uint3(n - 2u));
    float3 f = x - float3(i);
    uint sr = 3u, sg = n * 3u, sb = n * n * 3u;
    uint c000 = 8u + i.x * sr + i.y * sg + i.z * sb;
    uint c111 = c000 + sr + sg + sb;
    uint c1, c2;
    float4 w;
    if (f.x > f.y) {
        if (f.y > f.z)      { c1 = c000 + sr; c2 = c000 + sr + sg; w = float4(1.0f - f.x, f.x - f.y, f.y - f.z, f.z); }
        else if (f.x > f.z) { c1 = c000 + sr; c2 = c000 + sr + sb; w = float4(1.0f - f.x, f.x - f.z, f.z - f.y, f.y); }
        else                { c1 = c000 + sb; c2 = c000 + sr + sb; w = float4(1.0f - f.z, f.z - f.x, f.x - f.y, f.y); }
    } else {
        if (f.z > f.y)      { c1 = c000 + sb; c2 = c000 + sg + sb; w = float4(1.0f - f.z, f.z - f.y, f.y - f.x, f.x); }
        else if (f.z > f.x) { c1 = c000 + sg; c2 = c000 + sg + sb; w = float4(1.0f - f.y, f.y - f.z, f.z - f.x, f.x); }
        else                { c1 = c000 + sg; c2 = c000 + sr + sg; w = float4(1.0f - f.y, f.y - f.x, f.x - f.z, f.z); }
    }
    float3 o;
    for (uint k = 0; k < 3u; ++k)
        o[k] = w.x * L[c000 + k] + w.y * L[c1 + k] + w.z * L[c2 + k] + w.w * L[c111 + k];
    return o;
}

// Four consecutive output pixels per thread: 12 bytes is exactly three words,
// so every store is a whole word (byte stores would need 8-bit storage).
[shader("compute")]
[numthreads(WG, 1, 1)]
void yuv_to_rgb(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID,
                uniform YuvParams p) {
    uint quad = fold_index(gid, tid, p.groups_per_row, uint(WG));
    uint total = p.out_w * p.out_h;
    uint n_quads = (total + 3u) / 4u;
    if (quad >= n_quads) return;

    uint rot = (p.flags >> kF_RotShift) & 3u;
    uint sw = (rot & 1u) != 0u ? p.out_h : p.out_w;   // scaled-source dims
    uint sh = (rot & 1u) != 0u ? p.out_w : p.out_h;
    uint sub_x = (p.flags & kF_SubX) != 0u ? 1u : 0u;
    uint sub_y = (p.flags & kF_SubY) != 0u ? 1u : 0u;
    bool planar = (p.flags & kF_Planar) != 0u;
    uint shift = (p.flags >> kF_ShiftAt) & 0xFFu;

    uint words[3] = {0u, 0u, 0u};
    for (uint b = 0; b < 4u; ++b) {
        uint idx = quad * 4u + b;
        if (idx >= total) break;
        uint ox = idx % p.out_w;
        uint oy = idx / p.out_w;

        // Undo the rotation to reach coordinates in the scaled source.
        uint px, py;
        if (rot == 0u)      { px = ox;              py = oy; }
        else if (rot == 1u) { px = oy;              py = sh - 1u - ox; }
        else if (rot == 2u) { px = sw - 1u - ox;    py = sh - 1u - oy; }
        else                { px = sw - 1u - oy;    py = ox; }

        // Box-average the source footprint. At scale 1 this is a single texel.
        uint x0 = uint(float(px) * p.inv_sx);
        uint x1 = max(x0 + 1u, uint(float(px + 1u) * p.inv_sx));
        uint y0 = uint(float(py) * p.inv_sy);
        uint y1 = max(y0 + 1u, uint(float(py + 1u) * p.inv_sy));
        x1 = min(x1, p.src_w);
        y1 = min(y1, p.src_h);
        x0 = min(x0, x1 - 1u);
        y0 = min(y0, y1 - 1u);

        float y_sum = 0.0f;
        for (uint yy = y0; yy < y1; ++yy)
            for (uint xx = x0; xx < x1; ++xx)
                y_sum += load_sample(p.luma, yy * p.luma_stride + xx, p.flags, shift);
        float y_val = y_sum / float((x1 - x0) * (y1 - y0));

        uint cx0 = x0 >> sub_x, cy0 = y0 >> sub_y;
        uint cx1 = max(cx0 + 1u, (x1 + ((1u << sub_x) - 1u)) >> sub_x);
        uint cy1 = max(cy0 + 1u, (y1 + ((1u << sub_y) - 1u)) >> sub_y);
        float cb_sum = 0.0f, cr_sum = 0.0f;
        for (uint yy = cy0; yy < cy1; ++yy) {
            for (uint xx = cx0; xx < cx1; ++xx) {
                if (planar) {
                    cb_sum += load_sample(p.cb, yy * p.chroma_stride + xx, p.flags, shift);
                    cr_sum += load_sample(p.cr, yy * p.chroma_stride + xx, p.flags, shift);
                } else {
                    uint o = (yy * p.chroma_stride + xx) * 2u;
                    cb_sum += load_sample(p.cb, o, p.flags, shift);
                    cr_sum += load_sample(p.cb, o + 1u, p.flags, shift);
                }
            }
        }
        float inv_c = 1.0f / float((cx1 - cx0) * (cy1 - cy0));
        float4 v = float4(y_val, cb_sum * inv_c, cr_sum * inv_c, 1.0f);

        float3 rgb = clamp(float3(dot(p.m0, v), dot(p.m1, v), dot(p.m2, v)),
                           float3(0.0f), float3(255.0f));
        if ((p.flags & kF_Lut) != 0u)
            rgb = clamp(map_lut(p.lut, rgb * (1.0f / 255.0f)), float3(0.0f), float3(1.0f)) * 255.0f;
        uint r = uint(rgb.x + 0.5f);
        uint g = uint(rgb.y + 0.5f);
        uint bl = uint(rgb.z + 0.5f);

        uint byte0 = b * 3u;
        words[byte0 >> 2u] |= r << ((byte0 & 3u) * 8u);
        uint byte1 = byte0 + 1u;
        words[byte1 >> 2u] |= g << ((byte1 & 3u) * 8u);
        uint byte2 = byte0 + 2u;
        words[byte2 >> 2u] |= bl << ((byte2 & 3u) * 8u);
    }
    p.out[quad * 3u + 0u] = words[0];
    p.out[quad * 3u + 1u] = words[1];
    p.out[quad * 3u + 2u] = words[2];
}

// ---------------------------------------------------------------------------
// Sharpness: variance of the Laplacian on a fixed-size grey thumbnail
// ---------------------------------------------------------------------------
//
// The reference (reference/scripts/extract_frames.py) resizes to 512x512,
// takes the grey channel and reports var(Laplacian). Reproducing the size
// exactly keeps the numbers comparable; luma alone stands in for grey,
// which saves copying the chroma planes for a frame that may never be
// written out.

struct ThumbParams {
    float* out;      // out_w * out_h
    uint*  luma;
    uint   src_w;
    uint   src_h;
    uint   luma_stride;
    uint   out_w;
    uint   out_h;
    uint   flags;
    uint   shift;
    uint   groups_per_row;
};

[shader("compute")]
[numthreads(WG, 1, 1)]
void luma_thumbnail(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID,
                    uniform ThumbParams p) {
    uint i = fold_index(gid, tid, p.groups_per_row, uint(WG));
    if (i >= p.out_w * p.out_h) return;
    uint ox = i % p.out_w;
    uint oy = i / p.out_w;

    uint x0 = (ox * p.src_w) / p.out_w;
    uint x1 = max(x0 + 1u, ((ox + 1u) * p.src_w) / p.out_w);
    uint y0 = (oy * p.src_h) / p.out_h;
    uint y1 = max(y0 + 1u, ((oy + 1u) * p.src_h) / p.out_h);
    x1 = min(x1, p.src_w);
    y1 = min(y1, p.src_h);

    float sum = 0.0f;
    for (uint yy = y0; yy < y1; ++yy)
        for (uint xx = x0; xx < x1; ++xx)
            sum += load_sample(p.luma, yy * p.luma_stride + xx, p.flags, p.shift);
    p.out[i] = sum / float((x1 - x0) * (y1 - y0));
}

struct VarianceParams {
    float* out;      // one float, at index `slot`
    float* thumb;
    uint   size;
    uint   slot;
};

groupshared float g_sum[WG];
groupshared float g_sqr[WG];

// One workgroup for the whole 512x512 thumbnail: 256 threads x 1024 pixels is
// microseconds, and a single dispatch keeps the per-frame command count at two.
[shader("compute")]
[numthreads(WG, 1, 1)]
void laplacian_variance(uint3 tid : SV_GroupThreadID, uniform VarianceParams p) {
    uint t = tid.x;
    uint S = p.size;
    float sum = 0.0f, sqr = 0.0f;
    uint n = 0u;
    for (uint i = t; i < S * S; i += uint(WG)) {
        uint x = i % S;
        uint y = i / S;
        if (x == 0u || y == 0u || x + 1u >= S || y + 1u >= S) continue;
        float c = p.thumb[i];
        float lap = p.thumb[i - 1u] + p.thumb[i + 1u] + p.thumb[i - S] + p.thumb[i + S] -
                    4.0f * c;
        sum += lap;
        sqr += lap * lap;
        ++n;
    }
    g_sum[t] = sum;
    g_sqr[t] = sqr;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = uint(WG) / 2u; s > 0u; s >>= 1u) {
        if (t < s) {
            g_sum[t] += g_sum[t + s];
            g_sqr[t] += g_sqr[t + s];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (t == 0u) {
        float count = float((S - 2u) * (S - 2u));
        float mean = g_sum[0] / count;
        p.out[p.slot] = g_sqr[0] / count - mean * mean;
    }
}

// ---------------------------------------------------------------------------
// RGB -> NV12, for the encoder
// ---------------------------------------------------------------------------

struct Nv12Params {
    uint* rgb;         // width*height*3 bytes, rows top-down
    uint* luma;        // coded_w*coded_h bytes
    uint* chroma;      // coded_w*coded_h/2 bytes, Cb Cr interleaved
    uint  width;
    uint  height;
    uint  coded_w;     // multiples of 4 and 2; the padding repeats the edge
    uint  coded_h;
    uint  groups_per_row;
};

float3 load_rgb(uint* rgb, uint x, uint y, uint width) {
    uint i = (y * width + x) * 3u;
    float3 c;
    c.x = float((rgb[i >> 2u] >> ((i & 3u) * 8u)) & 0xFFu);
    ++i;
    c.y = float((rgb[i >> 2u] >> ((i & 3u) * 8u)) & 0xFFu);
    ++i;
    c.z = float((rgb[i >> 2u] >> ((i & 3u) * 8u)) & 0xFFu);
    return c * (1.0f / 255.0f);
}

// One 4x2 block per thread: a luma word from each row and one chroma word, so
// every store is whole. BT.709 in studio range, what the stream's VUI says.
[shader("compute")]
[numthreads(WG, 1, 1)]
void rgb_to_nv12(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID,
                 uniform Nv12Params p) {
    uint block = fold_index(gid, tid, p.groups_per_row, uint(WG));
    uint per_row = p.coded_w / 4u;
    if (block >= per_row * (p.coded_h / 2u)) return;
    uint bx = (block % per_row) * 4u;
    uint by = (block / per_row) * 2u;

    uint luma_words[2] = {0u, 0u};
    float2 chroma[2] = {float2(0.0f, 0.0f), float2(0.0f, 0.0f)};
    for (uint dy = 0u; dy < 2u; ++dy) {
        for (uint dx = 0u; dx < 4u; ++dx) {
            uint x = min(bx + dx, p.width - 1u);
            uint y = min(by + dy, p.height - 1u);
            float3 c = load_rgb(p.rgb, x, y, p.width);
            float Y = 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
            float Cb = (c.z - Y) / 1.8556f;
            float Cr = (c.x - Y) / 1.5748f;
            uint yb = uint(clamp(16.0f + 219.0f * Y, 0.0f, 255.0f) + 0.5f);
            luma_words[dy] |= yb << (dx * 8u);
            chroma[dx >> 1u] += float2(Cb, Cr) * 0.25f;
        }
    }
    p.luma[(by * p.coded_w + bx) >> 2u] = luma_words[0];
    p.luma[((by + 1u) * p.coded_w + bx) >> 2u] = luma_words[1];
    uint w = 0u;
    for (uint k = 0u; k < 2u; ++k) {
        uint cb = uint(clamp(128.0f + 224.0f * chroma[k].x, 0.0f, 255.0f) + 0.5f);
        uint cr = uint(clamp(128.0f + 224.0f * chroma[k].y, 0.0f, 255.0f) + 0.5f);
        w |= (cb | (cr << 8u)) << (k * 16u);
    }
    p.chroma[((by / 2u) * p.coded_w + bx) >> 2u] = w;
}
