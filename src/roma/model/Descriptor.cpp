#include "roma/model/Descriptor.h"

#include "nn/Ops.h"
#include "nn/core/Error.h"

#include <cmath>

namespace spirula::roma {
namespace {

using nn::Tensor;
using nn::DType;

Tensor take(nn::vk::Arena& a, int64_t n, int64_t c) { return nn::arena_tensor(a, DType::F32, n, c); }

nn::LinearOpts linear_options(const Weights& w, const std::string& p, nn::Act act = nn::Act::None) {
    nn::LinearOpts o;
    o.bias = w.get(p + ".bias"); o.act = act;
    return o;
}

}  // namespace

Tensor position_frequencies(nn::vk::Arena& arena, int height, int width, const std::vector<float>& periods) {
    const int quarter = (int)periods.size(), half = 2 * quarter;
    NN_CHECK(height > 0 && width > 0 && quarter > 0, "RoMa: invalid positional encoding dimensions");
    std::vector<float> host((size_t)height * width * half * 2);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            for (int i = 0; i < half; ++i) {
                const float coord = i < quarter ? 2.0f * (y + 0.5f) / height - 1.0f : 2.0f * (x + 0.5f) / width - 1.0f;
                const float angle = 6.283185307179586f * coord / periods[(size_t)i % quarter];
                const size_t at = ((size_t)y * width + x) * half * 2 + i * 2;
                host[at] = std::cos(angle); host[at + 1] = std::sin(angle);
            }
    Tensor out = take(arena, (int64_t)height * width, half * 2);
    nn::tensor_from_host(out, host.data(), (int64_t)host.size());
    return out;
}

void transformer_block(nn::vk::Arena& arena, const Weights& w, const std::string& p,
                       const Tensor& x, int heads, float epsilon, const Tensor& freqs,
                       int prefix_tokens, int batch, bool layer_scale) {
    nn::vk::ArenaScope scope(arena);
    const int64_t n = x.rows(), d = x.cols(), per = n / batch;
    Tensor t = take(arena, n, d);
    nn::layer_norm(t, x, w.get(p + ".norm1.weight"), w.get(p + ".norm1.bias"), epsilon);
    {
        nn::vk::ArenaScope attention_scope(arena);
        Tensor qkv = take(arena, n, 3 * d), attention = take(arena, n, d);
        nn::linear(qkv, t, w.get(p + ".attn.qkv.weight"), linear_options(w, p + ".attn.qkv"));
        if (freqs.valid()) {
            NN_CHECK(batch == 1 || prefix_tokens == 0, "RoMa: batched prefix RoPE unsupported");
            const int64_t count = per - prefix_tokens;
            const Tensor q = qkv.offsetElems((int64_t)prefix_tokens * 3 * d).view(n - prefix_tokens, 3 * d);
            nn::rope(q, freqs, heads, (int)(d / heads), count, batch, 3 * d, true);
            nn::rope(q.offsetElems(d), freqs, heads, (int)(d / heads), count, batch, 3 * d, true);
        }
        nn::AttnOpts o;
        o.allow_coop = w.mixedPrecision();
        o.n_heads = heads; o.head_dim = (int)(d / heads); o.batch = batch;
        o.q_stride = o.k_stride = o.v_stride = 3 * d; o.arena = &arena;
        nn::attention(attention, qkv, qkv.offsetElems(d), qkv.offsetElems(2 * d), per, per, o);
        nn::linear(t, attention, w.get(p + ".attn.proj.weight"), linear_options(w, p + ".attn.proj"));
    }
    if (layer_scale) nn::mul(t, t, w.get(p + ".ls1.gamma"));
    nn::add(x, x, t);
    nn::layer_norm(t, x, w.get(p + ".norm2.weight"), w.get(p + ".norm2.bias"), epsilon);
    {
        nn::vk::ArenaScope mlp_scope(arena);
        const Tensor first = w.get(p + ".mlp.fc1.weight");
        Tensor hidden = take(arena, n, first.shape[0]);
        nn::linear(hidden, t, first, linear_options(w, p + ".mlp.fc1", nn::Act::GeluErf));
        nn::linear(t, hidden, w.get(p + ".mlp.fc2.weight"), linear_options(w, p + ".mlp.fc2"));
    }
    if (layer_scale) nn::mul(t, t, w.get(p + ".ls2.gamma"));
    nn::add(x, x, t);
}

void descriptor(nn::vk::Arena& arena, const Weights& w, const Tensor& rgb,
                const std::array<Tensor, 2>& maps) {
    nn::vk::ArenaScope scope(arena);
    NN_CHECK(rgb.ndim == 3 && rgb.shape[2] == 3, "RoMa descriptor expects RGB [H,W,3]");
    const int gh = (int)rgb.shape[0] / 16, gw = (int)rgb.shape[1] / 16;
    const int64_t patches = (int64_t)gh * gw, n = patches + 5, d = 1024;
    NN_CHECK(gh > 0 && gw > 0, "RoMa descriptor image is too small");
    Tensor x = take(arena, n, d);
    nn::copy(x.slice0(0, 1), w.get("f.cls_token").view(1, d));
    nn::copy(x.slice0(1, 4), w.get("f.storage_tokens").view(4, d));
    {
        nn::vk::ArenaScope patch_scope(arena);
        Tensor normalized = nn::arena_tensor(arena, DType::F32, rgb.shape[0], rgb.shape[1], 3);
        nn::add(normalized, rgb, w.get("input.mean"), 1, -1);
        nn::div(normalized, normalized, w.get("input.std"));
        Tensor patch = take(arena, patches, 3 * 16 * 16);
        nn::patchify(patch, normalized, 16);
        nn::linear(x.slice0(5, patches), patch, w.get("f.patch_embed.proj.weight"), linear_options(w, "f.patch_embed.proj"));
    }
    Tensor freqs = position_frequencies(arena, gh, gw, w.descriptorPeriods());
    for (int block = 0; block < 18; ++block) {
        transformer_block(arena, w, "f.blocks." + std::to_string(block), x, 16, 1e-5f, freqs, 5, 1, true);
        if (block == 11 || block == 17) {
            const Tensor out = maps[block == 11 ? 0 : 1];
            NN_CHECK(out.numel() == patches * d, "RoMa descriptor output has incompatible shape");
            nn::layer_norm(out.view(patches, d), x.slice0(5, patches), w.get("f.norm.weight"), w.get("f.norm.bias"), 1e-5f);
        }
    }
}

}  // namespace spirula::roma
