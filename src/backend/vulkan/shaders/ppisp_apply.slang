// PPISP applied with the layout and clamp picked by the includer's kParamType /
// kClampOutput specialization constants, which it declares before including
// this. Shared by ppisp_image.slang and appearance_chain.slang.

static const int kMaxParams = PPISP_RQS_NUM_PARAMS;                 // 39
static const int kMaxRawLosses = int(RawPPISPRegLossIndexRQS::length);  // 23
static const int kNumWeighted = int(PPISPRegLossIndex::length);     // 6

[ForceInline]
int _pp_num_params() {
    if (kParamType == 0) return PPISP_NUM_PARAMS;
    if (kParamType == 1) return PPISP_RQS_NUM_PARAMS;
    if (kParamType == 2) return PPISP_NO_CRF_NUM_PARAMS;
    return PPISP_NO_CRF_NO_VIG_NUM_PARAMS;
}

[ForceInline]
int _pp_num_raw_losses() {
    if (kParamType == 0) return int(RawPPISPRegLossIndex::length);
    if (kParamType == 1) return int(RawPPISPRegLossIndexRQS::length);
    if (kParamType == 2) return int(RawPPISPRegLossIndexNoCRF::length);
    return int(RawPPISPRegLossIndexNoCRFNoVig::length);
}

[ForceInline]
bool _pp_finite(float v) {
    return (asuint(v) & 0x7f800000u) != 0x7f800000u;
}

[ForceInline]
float3 _pp_apply(float3 pixel, float2 pix_coord, float2 image_center,
                 float2 img_size, float pbuf[kMaxParams]) {
    if (kParamType == 0) {
        float ps[PPISP_NUM_PARAMS];
        [ForceUnroll]
        for (int i = 0; i < PPISP_NUM_PARAMS; i++) ps[i] = pbuf[i];
        return apply_ppisp(pixel, pix_coord, image_center, img_size, ps);
    } else if (kParamType == 1) {
        float ps[PPISP_RQS_NUM_PARAMS];
        [ForceUnroll]
        for (int i = 0; i < PPISP_RQS_NUM_PARAMS; i++) ps[i] = pbuf[i];
        return apply_ppisp_rqs(pixel, pix_coord, image_center, img_size, ps);
    } else if (kParamType == 2) {
        float ps[PPISP_NO_CRF_NUM_PARAMS];
        [ForceUnroll]
        for (int i = 0; i < PPISP_NO_CRF_NUM_PARAMS; i++) ps[i] = pbuf[i];
        return apply_ppisp_no_crf(pixel, pix_coord, image_center, img_size, ps,
                                  kClampOutput != 0);
    } else {
        float ps[PPISP_NO_CRF_NO_VIG_NUM_PARAMS];
        [ForceUnroll]
        for (int i = 0; i < PPISP_NO_CRF_NO_VIG_NUM_PARAMS; i++) ps[i] = pbuf[i];
        return apply_ppisp_no_crf_no_vig(pixel, pix_coord, image_center,
                                         img_size, ps, kClampOutput != 0);
    }
}

[ForceInline]
void _pp_apply_vjp(float3 pixel, float2 pix_coord, float2 image_center,
                   float2 img_size, float pbuf[kMaxParams], float3 v_out,
                   out float3 v_pixel, out float vbuf[kMaxParams]) {
    [ForceUnroll]
    for (int i = 0; i < kMaxParams; i++) vbuf[i] = 0.0f;
    if (kParamType == 0) {
        float ps[PPISP_NUM_PARAMS];
        [ForceUnroll]
        for (int i = 0; i < PPISP_NUM_PARAMS; i++) ps[i] = pbuf[i];
        float vs[PPISP_NUM_PARAMS];
        apply_ppisp_vjp(pixel, pix_coord, image_center, img_size, ps, v_out,
                        v_pixel, vs);
        [ForceUnroll]
        for (int i = 0; i < PPISP_NUM_PARAMS; i++) vbuf[i] = vs[i];
    } else if (kParamType == 1) {
        float ps[PPISP_RQS_NUM_PARAMS];
        [ForceUnroll]
        for (int i = 0; i < PPISP_RQS_NUM_PARAMS; i++) ps[i] = pbuf[i];
        float vs[PPISP_RQS_NUM_PARAMS];
        apply_ppisp_rqs_vjp(pixel, pix_coord, image_center, img_size, ps,
                            v_out, v_pixel, vs);
        [ForceUnroll]
        for (int i = 0; i < PPISP_RQS_NUM_PARAMS; i++) vbuf[i] = vs[i];
    } else if (kParamType == 2) {
        float ps[PPISP_NO_CRF_NUM_PARAMS];
        [ForceUnroll]
        for (int i = 0; i < PPISP_NO_CRF_NUM_PARAMS; i++) ps[i] = pbuf[i];
        float vs[PPISP_NO_CRF_NUM_PARAMS];
        apply_ppisp_no_crf_vjp(pixel, pix_coord, image_center, img_size, ps,
                               kClampOutput != 0, v_out, v_pixel, vs);
        [ForceUnroll]
        for (int i = 0; i < PPISP_NO_CRF_NUM_PARAMS; i++) vbuf[i] = vs[i];
    } else {
        float ps[PPISP_NO_CRF_NO_VIG_NUM_PARAMS];
        [ForceUnroll]
        for (int i = 0; i < PPISP_NO_CRF_NO_VIG_NUM_PARAMS; i++) ps[i] = pbuf[i];
        float vs[PPISP_NO_CRF_NO_VIG_NUM_PARAMS];
        apply_ppisp_no_crf_no_vig_vjp(pixel, pix_coord, image_center, img_size,
                                      ps, kClampOutput != 0, v_out, v_pixel, vs);
        [ForceUnroll]
        for (int i = 0; i < PPISP_NO_CRF_NO_VIG_NUM_PARAMS; i++) vbuf[i] = vs[i];
    }
}

