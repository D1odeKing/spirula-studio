#include "roma/model/Refiner.h"

#include "nn/Device.h"
#include "nn/Ops.h"
#include "nn/core/Error.h"
#include "nn/core/Log.h"
#include "nn/vk/Context.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

std::vector<float> read(const nn::Tensor& tensor) {
    std::vector<float> host((size_t)tensor.numel());
    nn::tensor_to_host(tensor, host.data(), tensor.numel());
    return host;
}

void dump(const nn::Tensor& tensor, const std::filesystem::path& root, const std::string& name) {
    auto host = read(tensor);
    double sq = 0;
    for (float value : host) {
        if (!std::isfinite(value)) throw nn::Error("nonfinite refinement output");
        sq += (double)value * value;
    }
    if (sq == 0) throw nn::Error("empty refinement output");
    std::printf("PASS %s RMS %.9g\n", name.c_str(), std::sqrt(sq / host.size()));
    if (!root.empty()) {
        std::filesystem::create_directories(root);
        std::ofstream out(root / (name + ".f32"), std::ios::binary);
        out.write((const char*)host.data(), (std::streamsize)(host.size() * sizeof(float)));
        if (!out) throw nn::Error("refinement dump write failed");
    }
}

void run(const spirula::roma::Weights& weights, const std::filesystem::path& root) {
    nn::vk::Arena arena("roma-refiner-test");
    arena.reserve(128ull << 20);
    const int h = 32, w = 48;
    const int channels[] = {64, 128, 256};
    std::array<nn::Tensor, 3> features[2];
    for (int view = 0; view < 2; ++view) {
        auto input = nn::arena_tensor(arena, nn::DType::F32, h, w, 3);
        std::vector<float> rgb((size_t)h * w * 3);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 3; ++c)
                    rgb[((size_t)y * w + x) * 3 + c] = (float)((17 * x + 31 * y + 73 * c + 43 * view) % 256) / 255;
        nn::tensor_from_host(input, rgb.data(), input.numel());
        for (int level = 0; level < 3; ++level)
            features[view][level] = nn::arena_tensor(arena, nn::DType::F32, h >> level, w >> level, channels[level]);
        spirula::roma::fine_features(arena, weights, input, features[view]);
        for (int level = 0; level < 3; ++level)
            dump(features[view][level], root, "vgg_" + std::to_string(view) + "_" + std::to_string(1 << level));
    }
    for (int level = 2; level >= 0; --level) {
        nn::vk::ArenaScope scope(arena);
        const int patch = 1 << level, fh = h >> level, fw = w >> level, cc = level == 2 ? 1 : 4;
        auto warp = nn::arena_tensor(arena, nn::DType::F32, fh, fw, 2);
        auto conf = nn::arena_tensor(arena, nn::DType::F32, fh, fw, cc, 1, 3);
        auto out_warp = nn::arena_tensor(arena, nn::DType::F32, fh, fw, 2);
        auto out_conf = nn::arena_tensor(arena, nn::DType::F32, fh, fw, 4);
        std::vector<float> host_warp((size_t)fh * fw * 2), host_conf((size_t)fh * fw * cc);
        for (int y = 0; y < fh; ++y)
            for (int x = 0; x < fw; ++x) {
                const size_t i = (size_t)y * fw + x;
                host_warp[i * 2] = 2.0f * (x + 0.5f) / fw - 1 + 0.083f;
                host_warp[i * 2 + 1] = 2.0f * (y + 0.5f) / fh - 1 - 0.047f;
                host_conf[i * cc] = 0.25f;
                if (cc == 4) { host_conf[i * cc + 1] = 1.2f; host_conf[i * cc + 2] = -0.05f; host_conf[i * cc + 3] = 0.8f; }
            }
        host_warp[0] = -1.05f;
        nn::tensor_from_host(warp, host_warp.data(), warp.numel());
        nn::tensor_from_host(conf, host_conf.data(), conf.numel());
        auto compute = [&] {
            spirula::roma::refine(arena, weights, patch, features[0][level], features[1][level],
                                  warp, conf, out_warp, out_conf, w / 512.0f, h / 512.0f);
        };
        compute();
        dump(out_warp, root, "refiner_" + std::to_string(patch) + "_warp");
        dump(out_conf, root, "refiner_" + std::to_string(patch) + "_confidence");
        auto narrow_warp = read(out_warp), narrow_conf = read(out_conf);
        for (size_t i = 0; i < narrow_conf.size(); i += 4) {
            const double xx = narrow_conf[i + 1], xy = narrow_conf[i + 2], yy = narrow_conf[i + 3];
            if (xx < 0 || yy < 0 || xx * yy < xy * xy - 1e-6 * (xx * yy + 1))
                throw nn::Error("refiner precision is not positive semidefinite");
        }
        if (nn::vk::Context::get().hasInt64()) {
            nn::set_wide_index(nn::WideIndex::Force);
            compute();
            nn::set_wide_index(nn::WideIndex::Auto);
            const auto wide_warp = read(out_warp), wide_conf = read(out_conf);
            for (size_t i = 0; i < wide_warp.size(); ++i)
                if (std::fabs(wide_warp[i] - narrow_warp[i]) > 1e-5f) throw nn::Error("wide refiner warp differs");
            for (size_t i = 0; i < wide_conf.size(); ++i)
                if (std::fabs(wide_conf[i] - narrow_conf[i]) > 1e-5f) throw nn::Error("wide refiner confidence differs");
            std::printf("PASS scale %d wide addressing\n", patch);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: roma_refiner_test CHECKPOINT [DUMP_DIRECTORY]\n"); return 2; }
    int result = 0;
    try {
        nn::set_coop_matrix_enabled(false);
        nn::set_log_level(1);
        spirula::roma::Weights weights;
        weights.load(argv[1]);
        run(weights, argc > 2 ? argv[2] : "");
    } catch (const std::exception& e) { std::printf("FAIL %s\n", e.what()); result = 1; }
    nn::shutdown();
    return result;
}
