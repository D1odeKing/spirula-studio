#include "roma/model/Matcher.h"
#include "roma/model/Head.h"
#include "nn/Device.h"
#include "nn/Ops.h"
#include "nn/core/Error.h"
#include "nn/core/Log.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

void check_output(const nn::Tensor& output, const std::filesystem::path& root, const std::string& name) {
    std::vector<float> host((size_t)output.numel());
    nn::tensor_to_host(output, host.data(), output.numel());
    double sq = 0;
    for (float value : host) {
        if (!std::isfinite(value)) throw nn::Error("nonfinite coarse matcher output");
        sq += (double)value * value;
    }
    if (sq == 0) throw nn::Error("empty coarse matcher output");
    std::printf("PASS %s RMS %.9g\n", name.c_str(), std::sqrt(sq / host.size()));
    if (!root.empty()) {
        std::filesystem::create_directories(root);
        std::ofstream out(root / (name + ".f32"), std::ios::binary);
        out.write((const char*)host.data(), (std::streamsize)(host.size() * sizeof(float)));
        if (!out) throw nn::Error("coarse matcher dump write failed");
    }
}

void run_case(const spirula::roma::Weights& weights, int h, int w, const std::filesystem::path& root) {
    nn::vk::Arena arena("roma-matcher-test");
    arena.reserve(128ull << 20);
    std::array<nn::Tensor, 2> features[2];
    for (int view = 0; view < 2; ++view)
        for (int tap = 0; tap < 2; ++tap) {
            auto& map = features[view][tap];
            map = nn::arena_tensor(arena, nn::DType::F32, h, w, 1024);
            std::vector<float> values((size_t)map.numel());
            for (size_t i = 0; i < values.size(); ++i)
                values[i] = (float)((int)((i * 13 + tap * 97 + view * 37) % 511) - 255) / 255.0f;
            nn::tensor_from_host(map, values.data(), map.numel());
        }
    auto ab = nn::arena_tensor(arena, nn::DType::F32, 4 * h, 4 * w, 3);
    auto ba = nn::arena_tensor(arena, nn::DType::F32, 4 * h, 4 * w, 3);
    const std::string prefix = "coarse_" + std::to_string(h) + "x" + std::to_string(w);
    spirula::roma::coarse_head(arena, weights, features[0], ab);
    check_output(ab, root, prefix + "_head");
    spirula::roma::coarse_match(arena, weights, features[0], features[1], ab, ba);
    check_output(ab, root, prefix + "_AB");
    check_output(ba, root, prefix + "_BA");
    std::vector<float> before((size_t)ab.numel()), after(before.size());
    nn::tensor_to_host(ab, before.data(), ab.numel());
    spirula::roma::coarse_match(arena, weights, features[0], features[1], ab);
    nn::tensor_to_host(ab, after.data(), ab.numel());
    for (size_t i = 0; i < before.size(); ++i)
        if (std::fabs(before[i] - after[i]) > 1e-5f) throw nn::Error("unidirectional coarse output changed");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: roma_matcher_test CHECKPOINT [DUMP_DIRECTORY]\n"); return 2; }
    int result = 0;
    try {
        nn::set_coop_matrix_enabled(false);
        nn::set_log_level(1);
        spirula::roma::Weights weights;
        weights.load(argv[1]);
        const std::filesystem::path root = argc > 2 ? argv[2] : "";
        run_case(weights, 2, 3, root);
        run_case(weights, 3, 5, root);
    } catch (const std::exception& e) { std::printf("FAIL %s\n", e.what()); result = 1; }
    nn::shutdown();
    return result;
}
