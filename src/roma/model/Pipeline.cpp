#include "roma/model/Pipeline.h"

#include "roma/model/Descriptor.h"
#include "roma/model/Matcher.h"
#include "roma/model/Refiner.h"
#include "roma/model/Layers.h"
#include "nn/core/Error.h"
#include "nn/io/StageDump.h"

namespace spirula::roma {
namespace {

PredictionMaps prediction(Arena& arena, int h, int w, int confidence_channels = 4) {
    return {map(arena, h, w, 2), map(arena, h, w, confidence_channels)};
}

void refinement_pass(Arena& arena, const Weights& weights, const Tensor& a, const Tensor& b,
                      const PredictionMaps& previous_ab, const PredictionMaps& previous_ba,
                      const PredictionMaps& out_ab, const PredictionMaps& out_ba,
                      bool high, const StageObserver& observer) {
    ArenaScope scope(arena);
    const int h = (int)a.shape[0], w = (int)a.shape[1];
    const bool bidirectional = out_ba.warp.valid();
    std::array<Tensor, 3> features[2];
    const int channels[] = {64, 128, 256};
    for (int view = 0; view < 2; ++view) {
        for (int level = 0; level < 3; ++level)
            features[view][level] = map(arena, h >> level, w >> level, channels[level]);
        fine_features(arena, weights, view ? b : a, features[view]);
    }
    PredictionMaps current[] = {previous_ab, previous_ba};
    for (int level = 2; level >= 0; --level) {
        const int patch = 1 << level, fh = h >> level, fw = w >> level;
        for (int direction = 0; direction < (bidirectional ? 2 : 1); ++direction) {
            PredictionMaps next = level == 0 ? (direction ? out_ba : out_ab) : prediction(arena, fh, fw);
            ArenaScope direction_scope(arena);
            const auto& prev = current[direction];
            PredictionMaps resized = prediction(arena, fh, fw, (int)prev.confidence.shape[2]);
            nn::resize_bilinear(resized.warp, prev.warp, false);
            nn::resize_bilinear(resized.confidence, prev.confidence, false);
            if (high && level == 2) {
                const float values[] = {1, 0, 0, 0};
                Tensor mask = nn::arena_tensor(arena, DType::F32, 4);
                nn::tensor_from_host(mask, values, 4);
                nn::mul(resized.confidence, resized.confidence, mask);
            }
            refine(arena, weights, patch, features[direction][level], features[1 - direction][level],
                    resized.warp, resized.confidence, next.warp, next.confidence, w / 512.0f, h / 512.0f);
            current[direction] = next;
            if (observer) {
                const std::string name = (high ? "high_" : "low_") + std::to_string(patch) + (direction ? "BA" : "AB");
                observer(name.c_str(), next);
            }
        }
    }
}

}  // namespace

void forward(Arena& arena, const Weights& weights, const Tensor& low_a, const Tensor& low_b,
              const Tensor& high_a, const Tensor& high_b,
              const PredictionMaps& output_ab, const PredictionMaps& output_ba,
              const StageObserver& observer,
              const DescriptorFeatures* descriptor_a, const DescriptorFeatures* descriptor_b) {
    ArenaScope scope(arena);
    auto valid_pair = [](const Tensor& a, const Tensor& b, int granularity) {
        return a.valid() && b.valid() && a.dtype == DType::F32 && b.dtype == DType::F32 &&
            a.ndim == 3 && b.ndim == 3 && a.shape[2] == 3 && b.shape[2] == 3 &&
            a.shape[0] == b.shape[0] && a.shape[1] == b.shape[1] && a.shape[0] > 0 && a.shape[1] > 0 &&
            a.shape[0] % granularity == 0 && a.shape[1] % granularity == 0;
    };
    NN_CHECK(valid_pair(low_a, low_b, 16), "RoMa low-resolution inputs must be equal RGB maps with dimensions divisible by 16");
    const bool high = high_a.valid() || high_b.valid();
    NN_CHECK(!high || valid_pair(high_a, high_b, 4), "RoMa high-resolution inputs must be equal RGB maps with dimensions divisible by 4");
    const Tensor& target = high ? high_a : low_a;
    auto valid_output = [&](const PredictionMaps& out) {
        const auto& warp = out.warp;
        const auto& confidence = out.confidence;
        return warp.valid() && confidence.valid() && warp.dtype == DType::F32 && confidence.dtype == DType::F32 &&
            warp.ndim == 3 && confidence.ndim == 3 && warp.shape[0] == target.shape[0] &&
            warp.shape[1] == target.shape[1] && confidence.shape[0] == target.shape[0] &&
            confidence.shape[1] == target.shape[1] && warp.shape[2] == 2 && confidence.shape[2] == 4;
    };
    NN_CHECK(valid_output(output_ab) && (!output_ba.warp.valid() || valid_output(output_ba)), "RoMa final output shapes differ");
    const bool bidirectional = output_ba.warp.valid();
    const int h = (int)low_a.shape[0], w = (int)low_a.shape[1];
    PredictionMaps coarse[] = {prediction(arena, h / 4, w / 4, 1), {}};
    if (bidirectional) coarse[1] = prediction(arena, h / 4, w / 4, 1);
    {
        ArenaScope matcher_scope(arena);
        std::array<Tensor, 2> features[2];
        nn::StageDump dump("ROMA_DUMP");
        const DescriptorFeatures* cached[] = {descriptor_a, descriptor_b};
        for (int view = 0; view < 2; ++view) {
            if (cached[view]) {
                features[view] = *cached[view];
                for (const auto& feature : features[view])
                    NN_CHECK(feature.valid() && feature.dtype == DType::F32 && feature.ndim == 3 &&
                             feature.shape[0] == h / 16 && feature.shape[1] == w / 16 && feature.shape[2] == 1024,
                             "RoMa cached descriptor shapes differ");
            } else {
                for (auto& feature : features[view]) feature = map(arena, h / 16, w / 16, 1024);
                descriptor(arena, weights, view ? low_b : low_a, features[view]);
            }
            for (int tap = 0; tap < 2; ++tap) {
                const std::string name = std::string("descriptor_") + (view ? "b_" : "a_") + std::to_string(tap);
                dump.tensor(name.c_str(), features[view][tap], {h / 16, w / 16, 1024});
            }
        }
        Tensor matches_ab = map(arena, h / 4, w / 4, 3), matches_ba;
        if (bidirectional) matches_ba = map(arena, h / 4, w / 4, 3);
        coarse_match(arena, weights, features[0], features[1], matches_ab, matches_ba);
        for (int direction = 0; direction < (bidirectional ? 2 : 1); ++direction) {
            const Tensor& packed = direction ? matches_ba : matches_ab;
            const int64_t n = (int64_t)h * w / 16;
            nn::strided_copy(coarse[direction].warp, packed, n, 2, 3, 2);
            nn::strided_copy(coarse[direction].confidence, packed.offsetElems(2), n, 1, 3, 1);
            if (observer) observer(direction ? "matcher_BA" : "matcher_AB", coarse[direction]);
        }
    }
    PredictionMaps low[] = {high ? prediction(arena, h, w) : output_ab, {}};
    if (bidirectional) low[1] = high ? prediction(arena, h, w) : output_ba;
    refinement_pass(arena, weights, low_a, low_b, coarse[0], coarse[1], low[0], low[1], false, observer);
    if (high) refinement_pass(arena, weights, high_a, high_b, low[0], low[1], output_ab, output_ba, true, observer);
}

}  // namespace spirula::roma
