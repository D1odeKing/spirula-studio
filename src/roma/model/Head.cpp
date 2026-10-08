#include "roma/model/Head.h"

#include "roma/model/Layers.h"

namespace spirula::roma {
namespace {

void residual_unit(Arena& arena, const Weights& weights, const std::string& name,
                   const Tensor& out, const Tensor& in) {
    ArenaScope scope(arena);
    Tensor activated = map(arena, in.shape[0], in.shape[1], in.shape[2]);
    Tensor tmp = map(arena, in.shape[0], in.shape[1], in.shape[2]);
    // DPT's in-place activation also rectifies the skip.
    nn::unary(out, in, Act::Relu);
    conv_layer(arena, weights, name + ".conv1", tmp, out);
    nn::unary(activated, tmp, Act::Relu);
    conv_layer(arena, weights, name + ".conv2", tmp, activated);
    nn::add(out, tmp, out);
}

void fusion(Arena& arena, const Weights& weights, const std::string& name,
            const Tensor& output, const Tensor& input, const Tensor& skip = {}) {
    ArenaScope scope(arena);
    Tensor x = map(arena, input.shape[0], input.shape[1], 256);
    nn::copy(x, input);
    if (skip.valid()) {
        Tensor residual = map(arena, skip.shape[0], skip.shape[1], 256);
        residual_unit(arena, weights, name + ".resConfUnit1", residual, skip);
        nn::add(x, x, residual);
    }
    residual_unit(arena, weights, name + ".resConfUnit2", x, x);
    Tensor resized = map(arena, output.shape[0], output.shape[1], 256);
    nn::resize_bilinear(resized, x, true);
    conv_layer(arena, weights, name + ".out_conv", output, resized);
}

}  // namespace

void coarse_head(Arena& arena, const Weights& weights,
                 const std::array<Tensor, 2>& features, const Tensor& output) {
    ArenaScope scope(arena);
    const int64_t gh = features[0].shape[0], gw = features[0].shape[1];
    const std::string head = "matcher.head.";
    const int64_t height[] = {4 * gh, 2 * gh, gh, (gh + 1) / 2};
    const int64_t width[] = {4 * gw, 2 * gw, gw, (gw + 1) / 2};
    const int channels[] = {256, 512, 1024, 1024};
    std::array<Tensor, 4> levels;
    for (int i = 0; i < 4; ++i) levels[i] = map(arena, height[i], width[i], 256);
    for (int i = 0; i < 4; ++i) {
        ArenaScope level_scope(arena);
        Tensor norm = map(arena, gh, gw, 1024);
        nn::layer_norm(norm, features[i / 2], weights.get(head + "norm.weight"), weights.get(head + "norm.bias"), 1e-5f);
        Tensor projected = map(arena, gh, gw, channels[i]);
        conv_layer(arena, weights, head + "projects." + std::to_string(i), projected, norm);
        Tensor resized = map(arena, height[i], width[i], channels[i]);
        const std::string layer = head + "resize_layers." + std::to_string(i);
        if (i < 2) nn::conv_transpose_patch(arena, resized, projected, weights.get(layer + ".weight"),
                                            weights.get(layer + ".bias"), i == 0 ? 4 : 2);
        else if (i == 2) nn::copy(resized, projected);
        else conv_layer(arena, weights, layer, resized, projected, 2);
        conv_layer(arena, weights, head + "scratch.layer" + std::to_string(i + 1) + "_rn", levels[i], resized);
    }
    Tensor x = map(arena, height[2], width[2], 256);
    fusion(arena, weights, head + "scratch.refinenet4", x, levels[3]);
    for (int i = 2; i >= 0; --i) {
        const int64_t h = i ? height[i - 1] : height[0] * 2;
        const int64_t w = i ? width[i - 1] : width[0] * 2;
        Tensor next = map(arena, h, w, 256);
        fusion(arena, weights, head + "scratch.refinenet" + std::to_string(i + 1), next, x, levels[i]);
        x = next;
    }
    Tensor first = map(arena, x.shape[0], x.shape[1], 128);
    conv_layer(arena, weights, head + "scratch.output_conv1", first, x);
    Tensor resized = map(arena, output.shape[0], output.shape[1], 128);
    nn::resize_bilinear(resized, first, true);
    Tensor hidden = map(arena, output.shape[0], output.shape[1], 32);
    conv_layer(arena, weights, head + "scratch.output_conv2.0", hidden, resized, 1, Act::Relu);
    conv_layer(arena, weights, head + "scratch.output_conv2.2", output, hidden);
}

}  // namespace spirula::roma
