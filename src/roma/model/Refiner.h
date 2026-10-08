#pragma once

#include "roma/model/Weights.h"

#include <array>

namespace spirula::roma {

// Feature outputs are full, half and quarter resolution, with 64/128/256 channels.
void fine_features(nn::vk::Arena& arena, const Weights& weights, const nn::Tensor& rgb,
                   const std::array<nn::Tensor, 3>& output);

// Warp uses normalized target (x,y); confidence is overlap logit and pixel precision (xx,xy,yy).
void refine(nn::vk::Arena& arena, const Weights& weights, int patch,
            const nn::Tensor& features_a, const nn::Tensor& features_b,
            const nn::Tensor& warp, const nn::Tensor& confidence,
            const nn::Tensor& output_warp, const nn::Tensor& output_confidence,
            float scale_x, float scale_y);

}  // namespace spirula::roma
