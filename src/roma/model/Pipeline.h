#pragma once

#include "roma/model/Weights.h"

#include <array>
#include <functional>

namespace spirula::roma {

struct PredictionMaps {
    nn::Tensor warp, confidence;
};

using StageObserver = std::function<void(const char*, const PredictionMaps&)>;
using DescriptorFeatures = std::array<nn::Tensor, 2>;

// All inputs and caller-owned outputs are float32; observer tensors live only during the callback.
void forward(nn::vk::Arena& arena, const Weights& weights,
              const nn::Tensor& low_a, const nn::Tensor& low_b,
              const nn::Tensor& high_a, const nn::Tensor& high_b,
              const PredictionMaps& output_ab, const PredictionMaps& output_ba = {},
              const StageObserver& observer = {},
              const DescriptorFeatures* descriptor_a = nullptr,
              const DescriptorFeatures* descriptor_b = nullptr);

}  // namespace spirula::roma
