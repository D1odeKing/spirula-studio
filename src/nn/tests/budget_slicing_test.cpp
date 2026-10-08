// An op's result must not depend on how the submit budget slices it. The
// budget is learned from timing, so a dependence there is run-to-run
// nondeterminism: attention once re-decided its key-range split per query
// slice, and a bf16 RoPE downstream turned that into 2 px of warp between runs.
// Each shape is run unsliced and at three budgets, and must match bit for bit.

#include "nn/Ops.h"
#include "nn/Tensor.h"
#include "nn/vk/Context.h"
#include "nn/vk/Memory.h"
#include "nn/vk/Pipelines.h"
#include "nn/vk/Stream.h"
#include "nn/vk/StreamTesting.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace {

int g_failures = 0;

std::vector<float> randn(size_t n, uint32_t seed) {
    std::mt19937 g(seed);
    std::normal_distribution<float> d(0.0f, 1.0f);
    std::vector<float> v(n);
    for (float& x : v) x = d(g);
    return v;
}

std::vector<float> host(const nn::Tensor& t) {
    std::vector<float> v((size_t)t.numel());
    nn::tensor_to_host(t, v.data(), t.numel());
    return v;
}

// `run` launches the op into `out`; `work` is its FLOP count as the op counts it.
template <class F>
void check_slicing(const char* name, double work, const nn::Tensor& out, F run) {
    nn::vk::testing::override_work_cap(1e30);
    run();
    const std::vector<float> ref = host(out);
    for (double div : {2.0, 7.0, 40.0}) {
        nn::vk::testing::override_work_cap(work / div);
        nn::fill(out, 0.0f);
        run();
        const std::vector<float> got = host(out);
        const bool same = std::memcmp(got.data(), ref.data(), ref.size() * 4) == 0;
        size_t diff = 0;
        for (size_t i = 0; i < ref.size(); ++i) diff += got[i] != ref[i];
        std::printf("  %s %s, budget = work/%g: %zu of %zu values differ\n",
                    same ? "ok  " : "FAIL", name, div, diff, ref.size());
        g_failures += !same;
    }
    nn::vk::testing::override_work_cap(-1);
}

}  // namespace

int main() {
    nn::vk::Context::get();
    nn::vk::Arena arena("budget-test");
    arena.reserve(512ull << 20);
    {
        struct Shape { const char* name; int64_t n; int heads, batch; };
        // Each splits its key range once sliced (fewer than 512 workgroups) but
        // not whole: a ViT backbone, a global block, and a batched call whose
        // 4-head items fall under the target one at a time but not together.
        for (const Shape& s : {Shape{"attention_backbone", 1605, 16, 1},
                               Shape{"attention_global", 3200, 12, 1},
                               Shape{"attention_per_image", 1600, 4, 2}}) {
            nn::vk::ArenaScope scope(arena);
            const int hd = 64;
            const int64_t D = (int64_t)s.heads * hd, rows = s.n * s.batch;
            nn::Tensor q = nn::arena_tensor(arena, nn::DType::F32, rows, 3 * D);
            nn::Tensor out = nn::arena_tensor(arena, nn::DType::F32, rows, D);
            const std::vector<float> hq = randn((size_t)(rows * 3 * D), 7);
            nn::tensor_from_host(q, hq.data(), (int64_t)hq.size());
            nn::AttnOpts ao;
            ao.n_heads = s.heads;
            ao.head_dim = hd;
            ao.batch = s.batch;
            ao.arena = &arena;
            ao.q_stride = ao.k_stride = ao.v_stride = 3 * D;
            const double work = 8.0 * s.n * s.n * hd * s.heads * s.batch;
            check_slicing(s.name, work, out, [&] {
                nn::attention(out, q, q.offsetElems(D), q.offsetElems(2 * D), s.n, s.n, ao);
            });
        }
    }
    {
        nn::vk::ArenaScope scope(arena);
        const int64_t M = 1605, N = 3072, K = 1024;
        nn::Tensor x = nn::arena_tensor(arena, nn::DType::F32, M, K);
        nn::Tensor w = nn::arena_tensor(arena, nn::DType::F32, N, K);
        nn::Tensor out = nn::arena_tensor(arena, nn::DType::F32, M, N);
        const std::vector<float> hx = randn((size_t)(M * K), 1), hw = randn((size_t)(N * K), 2);
        nn::tensor_from_host(x, hx.data(), (int64_t)hx.size());
        nn::tensor_from_host(w, hw.data(), (int64_t)hw.size());
        check_slicing("gemm", 2.0 * M * N * K, out, [&] { nn::linear(out, x, w); });
    }
    std::printf("%s\n", g_failures ? "FAIL" : "PASS");
    nn::vk::Stream::shutdown();
    nn::vk::Pipelines::get().shutdown();
    nn::vk::VramPool::get().releaseAll();
    return g_failures ? 1 : 0;
}
