#include "roma/Roma.h"
#include "roma/model/Pipeline.h"
#include "roma/model/Layers.h"

#include "nn/Device.h"
#include "nn/io/Resize.h"
#include "nn/core/Error.h"
#include "nn/core/Log.h"
#include "nn/vk/Memory.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {

std::vector<float> image(int w, int h, int view) {
    std::vector<float> rgb((size_t)w * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                rgb[((size_t)y * w + x) * 3 + c] = (float)((17 * x + 31 * y + 73 * c + 43 * view) % 256) / 255;
    return rgb;
}

void dump(const std::vector<float>& values, const std::filesystem::path& root, const std::string& name) {
    for (float v : values) NN_CHECK(std::isfinite(v), "nonfinite pipeline output");
    if (root.empty()) return;
    std::filesystem::create_directories(root);
    std::ofstream out(root / (name + ".f32"), std::ios::binary);
    out.write((const char*)values.data(), (std::streamsize)(values.size() * sizeof(float)));
    NN_CHECK((bool)out, "pipeline dump write failed");
}

void dump(const nn::Tensor& tensor, const std::filesystem::path& root, const std::string& name) {
    std::vector<float> values((size_t)tensor.numel());
    nn::tensor_to_host(tensor, values.data(), tensor.numel());
    dump(values, root, name);
}

void run_pipeline(const spirula::roma::Weights& weights, const std::filesystem::path& root) {
    using namespace spirula::roma;
    const auto a = image(53, 37, 0), b = image(61, 43, 1);
    Arena arena("roma-pipeline-test");
    MatchOptions options;
    options.low_width = 48; options.low_height = 32;
    options.high_width = 64; options.high_height = 48;
    arena.reserve(Session::plannedScratchBytes(options));
    for (int test_case = 0; test_case < 3; ++test_case) {
        ArenaScope scope(arena);
        const bool high = test_case > 0, bidirectional = test_case < 2;
        const int hw = test_case == 2 ? 24 : 64, hh = test_case == 2 ? 16 : 48;
        const std::string prefix = "case" + std::to_string(test_case) + "_";
        auto input = [&](const std::vector<float>& rgb, int w, int h, int wo, int ho, const std::string& name) {
            const auto resized = nn::resize_rgb_bicubic(rgb.data(), w, h, wo, ho, nn::BicubicWeights::Float32);
            auto tensor = map(arena, ho, wo, 3);
            nn::tensor_from_host(tensor, resized.data(), tensor.numel());
            dump(resized, root, prefix + name);
            return tensor;
        };
        const auto low_a = input(a, 53, 37, 48, 32, "low_a"), low_b = input(b, 61, 43, 48, 32, "low_b");
        nn::Tensor high_a, high_b;
        if (high) {
            high_a = input(a, 53, 37, hw, hh, "high_a");
            high_b = input(b, 61, 43, hw, hh, "high_b");
        }
        const int w = high ? hw : 48, h = high ? hh : 32;
        PredictionMaps ab{map(arena, h, w, 2), map(arena, h, w, 4)}, ba;
        if (bidirectional) ba = {map(arena, h, w, 2), map(arena, h, w, 4)};
        forward(arena, weights, low_a, low_b, high_a, high_b, ab, ba,
                  [&](const char* stage, const PredictionMaps& pred) {
                      dump(pred.warp, root, prefix + stage + "_warp");
                      dump(pred.confidence, root, prefix + stage + "_confidence");
                  });
        std::printf("PASS pipeline case %d (%dx%d, directions %d)\n", test_case, w, h, bidirectional ? 2 : 1);
    }
    std::printf("Pipeline peak scratch %llu bytes\n", (unsigned long long)arena.highWater());
}

template<class F> void rejects(F action) {
    bool threw = false;
    try { action(); } catch (const std::exception&) { threw = true; }
    NN_CHECK(threw, "expected invalid RoMa request to be rejected");
}

void identical(const spirula::roma::PairPrediction& a, const spirula::roma::PairPrediction& b) {
    for (const auto& pair : {std::make_pair(&a.forward, &b.forward), std::make_pair(&a.backward, &b.backward)}) {
        NN_CHECK(pair.first->width == pair.second->width && pair.first->height == pair.second->height &&
                 pair.first->warp == pair.second->warp && pair.first->overlap == pair.second->overlap &&
                 pair.first->precision == pair.second->precision, "feature caching changed the prediction");
    }
}

void run_cached(spirula::roma::Session& session, const spirula::roma::MatchOptions& options,
                  const std::vector<float>& a, const std::vector<float>& b,
                  const spirula::roma::PairPrediction& expected) {
    using namespace spirula::roma;
    auto match = [&](uint64_t id_a, uint64_t id_b, const MatchOptions& opts) {
        return session.matchCached(id_a, a.data(), 53, 37, id_b, b.data(), 61, 43, opts);
    };
    identical(expected, match(1, 2, options));
    const auto cold = session.featureCacheStatistics();
    identical(expected, match(1, 2, options));
    NN_CHECK(session.featureCacheStatistics().hits >= 2 && cold.bytes > 0, "feature cache did not reuse images");
    const auto reverse = session.match(b.data(), 61, 43, a.data(), 53, 37, options);
    identical(reverse, session.matchCached(2, b.data(), 61, 43, 1, a.data(), 53, 37, options));
    auto tight = options;
    tight.memory_budget_bytes = session.deviceBytes() - cold.bytes;
    identical(expected, match(1, 2, tight));
    NN_CHECK(session.featureCacheStatistics().bytes == 0 && session.deviceBytes() <= tight.memory_budget_bytes,
             "feature cache exceeded the device budget");
    session.setFeatureCacheBudget(cold.bytes / 2);
    identical(expected, match(3, 4, options));
    identical(expected, match(5, 6, options));
    NN_CHECK(session.featureCacheStatistics().evictions > 0 && session.featureCacheStatistics().bytes <= cold.bytes / 2,
             "feature cache eviction exceeded its budget");
    session.setFeatureCacheBudget();
    session.clearFeatureCache();
    NN_CHECK(session.featureCacheStatistics().bytes == 0, "feature cache clear retained allocations");
    auto cancelled = options;
    cancelled.progress = [](const char* stage) { if (std::string(stage) == "descriptor") throw nn::Error("cancel cache preparation"); };
    rejects([&] { match(1, 2, cancelled); });
    identical(expected, match(1, 2, options));
    cancelled.progress = [](const char* stage) { if (std::string(stage) == "matcher_AB") throw nn::Error("cancel cached inference"); };
    rejects([&] { match(1, 2, cancelled); });
    identical(expected, match(1, 2, options));
    auto low = options;
    low.low_width = 32; low.high_width = low.high_height = 0; low.bidirectional = false;
    identical(session.match(a.data(), 53, 37, b.data(), 61, 43, low), match(1, 2, low));
    identical(expected, match(1, 2, options));
    session.setFeatureCacheBudget(0);
    identical(expected, match(1, 2, options));
    session.setFeatureCacheBudget();
    rejects([&] { match(1, 1, options); });
    std::printf("PASS cached prediction equality, reverse reuse, eviction, budget fallback, grid changes, cancellation\n");
}

void run_session(const std::string& checkpoint, const std::filesystem::path& root,
                   spirula::roma::InferencePrecision precision = spirula::roma::InferencePrecision::Float32) {
    using namespace spirula::roma;
    const auto a = image(53, 37, 0), b = image(61, 43, 1);
    const auto before = nn::vk::Allocator::get().totalBytes();
    Session session;
    rejects([&] { session.match(a.data(), 53, 37, b.data(), 61, 43); });
    session.load(checkpoint, precision);
    NN_CHECK(session.precision() == Session::resolvePrecision(precision), "session precision resolution differs");
    MatchOptions opts;
    opts.low_width = 48; opts.low_height = 32; opts.high_width = 64; opts.high_height = 48;
    opts.precision = session.precision() == InferencePrecision::Float32 ? InferencePrecision::Mixed : InferencePrecision::Float32;
    rejects([&] { session.match(a.data(), 53, 37, b.data(), 61, 43, opts); });
    opts.precision = InferencePrecision::Automatic;
    opts.memory_budget_bytes = 1;
    rejects([&] { session.match(a.data(), 53, 37, b.data(), 61, 43, opts); });
    opts.memory_budget_bytes = 0;
    opts.progress = [](const char* stage) { if (std::string(stage) == "matcher_AB") throw nn::Error("cancel test"); };
    rejects([&] { session.match(a.data(), 53, 37, b.data(), 61, 43, opts); });
    opts.progress = {};
    const auto pred = session.match(a.data(), 53, 37, b.data(), 61, 43, opts);
    for (const auto& direction : {std::make_pair(&pred.forward, "AB"), std::make_pair(&pred.backward, "BA")}) {
        NN_CHECK(direction.first->width == 64 && direction.first->height == 48, "session returned incorrect dimensions");
        for (float v : direction.first->overlap) NN_CHECK(v >= 0 && v <= 1, "invalid overlap probability");
        dump(direction.first->warp, root, std::string("session_") + direction.second + "_warp");
        dump(direction.first->overlap, root, std::string("session_") + direction.second + "_overlap");
        dump(direction.first->precision, root, std::string("session_") + direction.second + "_precision");
    }
    NN_CHECK(session.peakScratchBytes() <= Session::plannedScratchBytes(opts), "session memory plan was exceeded");
    run_cached(session, opts, a, b, pred);
    auto saturated = opts;
    saturated.high_width = saturated.high_height = 0;
    saturated.bidirectional = false;
    saturated.overlap_saturation = 0;
    const auto saturated_pred = session.match(a.data(), 53, 37, b.data(), 61, 43, saturated);
    NN_CHECK(saturated_pred.backward.warp.empty(), "unidirectional session returned a reverse prediction");
    for (float probability : saturated_pred.forward.overlap) NN_CHECK(probability == 1, "overlap saturation differs from upstream");
    session.unload();
    NN_CHECK(!session.loaded() && session.deviceBytes() == 0, "session unload retained memory");
    NN_CHECK(nn::vk::Allocator::get().totalBytes() <= before + (8ull << 20), "session leaked allocations");
    auto bad = opts; bad.low_width = 47;
    rejects([&] { bad.validate(); });
    bad = opts; bad.high_height = 0;
    rejects([&] { bad.validate(); });
    bad = opts; bad.low_width = 8192; bad.low_height = 8192;
    rejects([&] { bad.validate(); });
    NN_CHECK(MatchOptions::preset("turbo").low_width == 320 && !MatchOptions::preset("base").bidirectional &&
             MatchOptions::preset("fast").high_width == 0 && MatchOptions::preset("precise").high_width == 1280,
             "RoMa presets differ from upstream");
    std::printf("PASS session output, resource budget, cancellation recovery, unload, options\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: roma_pipeline_test CHECKPOINT [DUMP_DIRECTORY]\n"); return 2; }
    int result = 0;
    try {
        nn::set_log_level(1);
        const std::filesystem::path root = argc > 2 ? argv[2] : "";
        {
            spirula::roma::Weights weights;
            weights.load(argv[1]);
            run_pipeline(weights, root);
        }
        run_session(argv[1], root);
        run_session(argv[1], root.empty() ? root : root / "mixed", spirula::roma::InferencePrecision::Mixed);
    } catch (const std::exception& e) { std::printf("FAIL %s\n", e.what()); result = 1; }
    nn::shutdown();
    return result;
}
