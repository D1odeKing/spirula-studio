#pragma once

#include <vector>

namespace nn {

enum class BicubicWeights { Pillow, Float32 };
// Interleaved float RGB; bicubic antialiasing with half-pixel coordinates.
std::vector<float> resize_rgb_bicubic(const float* rgb, int width, int height,
                                       int output_width, int output_height,
                                       BicubicWeights weights = BicubicWeights::Pillow);

}  // namespace nn
