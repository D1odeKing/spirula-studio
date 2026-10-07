#pragma once

#include "roma/model/Weights.h"

#include <array>

namespace spirula::roma {

// Coarse [H/4,W/4,3] prediction: normalized target coordinates and overlap logit.
void coarse_match(nn::vk::Arena& arena, const Weights& weights,
                  const std::array<nn::Tensor, 2>& features_a,
                  const std::array<nn::Tensor, 2>& features_b,
                  const nn::Tensor& output_ab, const nn::Tensor& output_ba = {});

}  // namespace spirula::roma
