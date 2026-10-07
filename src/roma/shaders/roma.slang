#include "_common.slang"

struct DisplacementParams {
    float* out;
    uint* warp;
    uint h, w, groups_per_row;
    float scale_x, scale_y;
};

[shader("compute")]
[numthreads(256, 1, 1)]
void displacement(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID,
                  uniform DisplacementParams p) {
    uint i = fold_index(gid, tid, p.groups_per_row, 256);
    if (i >= p.h * p.w) return;
    float gx = 2.0f * (float(i % p.w) + 0.5f) / float(p.w) - 1.0f;
    float gy = 2.0f * (float(i / p.w) + 0.5f) / float(p.h) - 1.0f;
    store_at(p.out, i * 2, (load_at(p.warp, i * 2, false) - gx) * p.scale_x);
    store_at(p.out, i * 2 + 1, (load_at(p.warp, i * 2 + 1, false) - gy) * p.scale_y);
}

struct UpdateParams {
    float* out_warp;
    float* out_conf;
    uint* warp;
    uint* confidence;
    uint* delta_warp;
    uint* delta_conf;
    uint h, w, confidence_channels, groups_per_row;
};

float softplus(float x) { return max(x, 0.0f) + log(1.0f + exp(-abs(x))); }

[shader("compute")]
[numthreads(256, 1, 1)]
void update(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uniform UpdateParams p) {
    uint i = fold_index(gid, tid, p.groups_per_row, 256);
    if (i >= p.h * p.w) return;
    float wx = load_at(p.warp, 2 * i, false) + load_at(p.delta_warp, 2 * i, false) / (4.0f * float(p.w));
    float wy = load_at(p.warp, 2 * i + 1, false) + load_at(p.delta_warp, 2 * i + 1, false) / (4.0f * float(p.h));
    float l00 = softplus(load_at(p.delta_conf, 4 * i + 1, false)) + 1e-6f;
    float l10 = load_at(p.delta_conf, 4 * i + 2, false);
    float l11 = softplus(load_at(p.delta_conf, 4 * i + 3, false)) + 1e-6f;
    float prev_overlap = load_at(p.confidence, i * p.confidence_channels, false);
    float prev_xx = 0, prev_xy = 0, prev_yy = 0;
    if (p.confidence_channels == 4) {
        prev_xx = load_at(p.confidence, 4 * i + 1, false);
        prev_xy = load_at(p.confidence, 4 * i + 2, false);
        prev_yy = load_at(p.confidence, 4 * i + 3, false);
    }
    store_at(p.out_warp, i * 2, wx);
    store_at(p.out_warp, i * 2 + 1, wy);
    store_at(p.out_conf, i * 4, prev_overlap + load_at(p.delta_conf, 4 * i, false));
    store_at(p.out_conf, i * 4 + 1, prev_xx + l00 * l00);
    store_at(p.out_conf, i * 4 + 2, prev_xy + l00 * l10);
    store_at(p.out_conf, i * 4 + 3, prev_yy + l10 * l10 + l11 * l11);
}
