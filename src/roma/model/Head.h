#pragma once

#include "roma/model/Weights.h"

#include <array>

namespace spirula::roma {

void coarse_head(nn::vk::Arena& arena, const Weights& weights,
                 const std::array<nn::Tensor, 2>& features, const nn::Tensor& output);

}  // namespace spirula::roma
