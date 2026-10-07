#pragma once

#include "roma/model/Weights.h"
#include "nn/Ops.h"

namespace spirula::roma {

using nn::Tensor;
using nn::DType;
using nn::Act;
using Arena = nn::vk::Arena;
using ArenaScope = nn::vk::ArenaScope;

inline Tensor map(Arena& arena, int64_t h, int64_t w, int64_t c) {
    return nn::arena_tensor(arena, DType::F32, h, w, c, 1, 3);
}

inline void linear_layer(const Weights& weights, const std::string& name,
                         const Tensor& out, const Tensor& in, Act act = Act::None) {
    nn::LinearOpts o;
    if (weights.has(name + ".bias")) o.bias = weights.get(name + ".bias");
    o.act = act;
    nn::linear(out, in, weights.get(name + ".weight"), o);
}

inline void conv_layer(Arena& arena, const Weights& weights, const std::string& name,
                       const Tensor& out, const Tensor& in, int stride = 1, Act act = Act::None) {
    const Tensor kernel = weights.get(name + ".weight");
    nn::ConvOpts o;
    o.pad_y = (int)kernel.shape[2] / 2; o.pad_x = (int)kernel.shape[3] / 2;
    o.stride_x = o.stride_y = stride; o.act = act;
    if (weights.has(name + ".bias")) o.bias = weights.get(name + ".bias");
    nn::conv2d(arena, out, in, kernel, (int)kernel.shape[2], (int)kernel.shape[3], o);
}

inline void batch_norm(const Weights& weights, const std::string& name,
                       const Tensor& out, const Tensor& in, Act act = Act::None) {
    nn::mul(out, in, weights.get(name + ".scale"));
    nn::add(out, out, weights.get(name + ".shift"), 1, 1, act);
}

}  // namespace spirula::roma
