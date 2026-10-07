#pragma once

#include "roma/model/Weights.h"

#include <array>

namespace spirula::roma {

// DINOv3-L/16 taps at blocks 11 and 17, normalized without prefix tokens.
void descriptor(nn::vk::Arena& arena, const Weights& weights, const nn::Tensor& rgb,
                const std::array<nn::Tensor, 2>& maps);

void transformer_block(nn::vk::Arena& arena, const Weights& weights, const std::string& prefix,
                       const nn::Tensor& tokens, int heads, float epsilon,
                       const nn::Tensor& frequencies = {}, int prefix_tokens = 0,
                       int batch = 1, bool layer_scale = false);

nn::Tensor position_frequencies(nn::vk::Arena& arena, int height, int width,
                               const std::vector<float>& periods);

}  // namespace spirula::roma
