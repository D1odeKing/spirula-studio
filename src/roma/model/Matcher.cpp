#include "roma/model/Matcher.h"

#include "roma/model/Descriptor.h"
#include "roma/model/Head.h"
#include "roma/model/Layers.h"
#include "nn/core/Error.h"
#include "nn/io/StageDump.h"

#include <algorithm>
#include <cmath>

namespace spirula::roma {
namespace {

Tensor embedding_basis(Arena& arena, const Weights& weights, int height, int width) {
    const auto& omega = weights.matcherOmega();
    const float scale = weights.matcherScale();
    const int64_t n = (int64_t)height * width;
    std::vector<float> host((size_t)n * 1024);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const float gx = 2.0f * (x + 0.5f) / width - 1.0f;
            const float gy = 2.0f * (y + 0.5f) / height - 1.0f;
            const size_t token = (size_t)y * width + x;
            for (int j = 0; j < 512; ++j) {
                const float v = scale * (omega[j * 2] * gx + omega[j * 2 + 1] * gy);
                host[(size_t)j * n + token] = std::sin(v);
                host[(size_t)(j + 512) * n + token] = std::cos(v);
            }
        }
    Tensor out = nn::arena_tensor(arena, DType::F32, 1024, n);
    nn::tensor_from_host(out, host.data(), (int64_t)host.size());
    return out;
}

void match_embedding(Arena& arena, const Tensor& source, const Tensor& target,
                     const Tensor& basis, const Tensor& output, float temperature) {
    ArenaScope scope(arena);
    const int64_t n = source.rows();
    Tensor normalized_a = nn::arena_tensor(arena, DType::F32, n, 1024);
    Tensor normalized_b = nn::arena_tensor(arena, DType::F32, n, 1024);
    nn::l2_normalize_rows(normalized_a, source);
    nn::l2_normalize_rows(normalized_b, target);
    for (int64_t start = 0; start < n; start += 128) {
        ArenaScope tile_scope(arena);
        const int64_t count = std::min<int64_t>(128, n - start);
        Tensor scores = nn::arena_tensor(arena, DType::F32, count, n);
        nn::matmul_nt(scores, normalized_a.slice0(start, count), normalized_b, 1.0f / temperature);
        nn::softmax_rows(scores, scores);
        nn::matmul_nt(output.slice0(start, count), scores, basis);
    }
}

}  // namespace

void coarse_match(Arena& arena, const Weights& weights,
                  const std::array<Tensor, 2>& features_a, const std::array<Tensor, 2>& features_b,
                  const Tensor& output_ab, const Tensor& output_ba) {
    ArenaScope scope(arena);
    for (int tap = 0; tap < 2; ++tap) {
        const Tensor& a = features_a[tap];
        const Tensor& b = features_b[tap];
        NN_CHECK(a.valid() && b.valid() && a.ndim == 3 && b.ndim == 3 &&
                 a.dtype == DType::F32 && b.dtype == DType::F32 &&
                 a.shape[0] == features_a[0].shape[0] && a.shape[1] == features_a[0].shape[1] &&
                 b.shape[0] == a.shape[0] && b.shape[1] == a.shape[1] &&
                 a.shape[2] == 1024 && b.shape[2] == 1024,
                 "RoMa matcher expects two equal-sized descriptor maps per image");
    }
    const int gh = (int)features_a[0].shape[0], gw = (int)features_a[0].shape[1];
    for (const Tensor& out : {output_ab, output_ba}) {
        if (!out.valid()) continue;
        NN_CHECK(out.dtype == DType::F32 && out.ndim == 3 && out.shape[0] == 4 * gh &&
                 out.shape[1] == 4 * gw && out.shape[2] == 3, "RoMa matcher output must be [4H,4W,3]");
    }
    NN_CHECK(output_ab.valid() && gh > 0 && gw > 0, "RoMa matcher requires a forward output and nonempty inputs");
    const int64_t n = (int64_t)gh * gw;
    Tensor input = nn::arena_tensor(arena, DType::F32, 2 * n, 2048);
    for (int tap = 0; tap < 2; ++tap) {
        nn::strided_copy(input.offsetElems(tap * 1024), features_a[tap], n, 1024, 1024, 2048);
        nn::strided_copy(input.offsetElems(n * 2048 + tap * 1024), features_b[tap], n, 1024, 1024, 2048);
    }
    Tensor x = nn::arena_tensor(arena, DType::F32, 2 * n, 768);
    linear_layer(weights, "matcher.mv_vit.projector", x, input);
    nn::StageDump dump("ROMA_DUMP");
    dump.tensor("matcher_projected", x, {2, gh, gw, 768});
    Tensor freqs = position_frequencies(arena, gh, gw, weights.matcherPeriods());
    for (int block = 0; block < 12; ++block) {
        transformer_block(arena, weights, "matcher.mv_vit.blocks." + std::to_string(block), x, 12, 1e-6f,
                          block % 2 ? freqs : Tensor{}, 0, block % 2 ? 2 : 1);
        dump.tensor(("matcher_block_" + std::to_string(block)).c_str(), x, {2, gh, gw, 768});
    }
    Tensor norm = nn::arena_tensor(arena, DType::F32, 2 * n, 768);
    nn::layer_norm(norm, x, weights.get("matcher.mv_vit.norm.weight"), weights.get("matcher.mv_vit.norm.bias"), 1e-6f);
    Tensor mv = nn::arena_tensor(arena, DType::F32, 2 * n, 1024);
    linear_layer(weights, "matcher.mv_vit.output_projector", mv, norm);
    dump.tensor("matcher_mv", mv, {2, gh, gw, 1024});
    Tensor basis = embedding_basis(arena, weights, gh, gw);
    const float temperature = weights.matcherTemperature();
    NN_CHECK(std::isfinite(temperature) && temperature > 0, "RoMa matcher temperature must be positive");
    for (int direction = 0; direction < (output_ba.valid() ? 2 : 1); ++direction) {
        ArenaScope direction_scope(arena);
        Tensor source = mv.slice0(direction * n, n), target = mv.slice0((1 - direction) * n, n);
        Tensor context = map(arena, gh, gw, 1024);
        match_embedding(arena, source, target, basis, context.view(n, 1024), temperature);
        const std::string direction_name = direction ? "BA" : "AB";
        dump.tensor(("embedding_" + direction_name).c_str(), context, {gh, gw, 1024});
        const auto& features = direction ? features_b : features_a;
        nn::add(context, context, source.view(gh, gw, 1024));
        nn::add(context, context, features[1]);
        coarse_head(arena, weights, {features[0], context}, direction ? output_ba : output_ab);
    }
}

}  // namespace spirula::roma
