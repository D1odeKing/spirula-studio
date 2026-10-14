#pragma once

// Spherical-beta lobes, the view-dependent colour of Deformable Beta
// Splatting (Liu et al., SIGGRAPH 2025). Each lobe adds
//
//     softplus(rgb) * max(dot(dir, axis(theta, phi)), 0) ^ (4 exp(beta))
//
// to the clamped SH colour, `dir` being the unit direction from the camera to
// the splat as for SH. core/SphericalBeta.h is the host copy, held to this one
// by sb_parity; docs/notes/spherical-beta.md has the rest.

// The trainers' softplus sharpness, 10 ln 2, which makes softplus(0) = 0.1.
static const float kSbSoftplusBeta = 6.9314718055994530f;

[ForceInline]
float sb_softplus(float x) {
    float bx = kSbSoftplusBeta * x;
    return (max(bx, 0.0f) + log(1.0f + exp(-abs(bx)))) / kSbSoftplusBeta;
}

[ForceInline]
float3 sb_softplus3(float3 x) {
    return float3(sb_softplus(x.x), sb_softplus(x.y), sb_softplus(x.z));
}

[ForceInline]
float3 sb_softplus3_grad(float3 x) {
    return float3(1.0f) / (float3(1.0f) + exp(-kSbSoftplusBeta * x));
}

[ForceInline]
float3 sb_axis(float theta, float phi) {
    float st = sin(theta);
    return float3(st * cos(phi), st * sin(phi), cos(theta));
}

// One lobe at unit `dir`; tpb = (theta, phi, beta).
[ForceInline]
float3 sb_lobe_color(float3 dir, float3 rgb, float3 tpb) {
    float d = dot(dir, sb_axis(tpb.x, tpb.y));
    if (!(d > 0.0f))
        return float3(0.0f);
    return pow(d, 4.0f * exp(tpb.z)) * sb_softplus3(rgb);
}

// Adds one lobe's gradients; v_dir is with respect to the unit direction.
// For n < 1 the slope n d^(n-1) is unbounded at the hemisphere's edge, where
// an fp32 dot is rounding noise: it is taken no nearer than d = 1e-3 (<= 53).
[ForceInline]
void sb_lobe_vjp(float3 dir, float3 rgb, float3 tpb, float3 v_color,
                 inout float3 v_rgb, inout float3 v_tpb, inout float3 v_dir) {
    float st = sin(tpb.x), ct = cos(tpb.x);
    float sp = sin(tpb.y), cp = cos(tpb.y);
    float3 axis = float3(st * cp, st * sp, ct);
    float d = dot(dir, axis);
    if (!(d > 0.0f))
        return;
    float n = 4.0f * exp(tpb.z);
    float log_d = log(d);
    float w = exp(n * log_d);
    v_rgb += (w * v_color) * sb_softplus3_grad(rgb);
    float s = dot(v_color, sb_softplus3(rgb));
    float log_ds = n < 1.0f ? max(log_d, -6.9077553f) : log_d;
    float g = s * n * exp((n - 1.0f) * log_ds);
    v_dir += g * axis;
    v_tpb.x += g * dot(dir, float3(ct * cp, ct * sp, -st));
    v_tpb.y += g * (dir.y * st * cp - dir.x * st * sp);
    v_tpb.z += s * w * n * log_d;
}
