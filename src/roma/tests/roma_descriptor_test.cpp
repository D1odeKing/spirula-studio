#include "roma/model/Descriptor.h"

#include "nn/Device.h"
#include "nn/Ops.h"
#include "nn/core/Error.h"
#include "nn/core/Log.h"
#include "nn/vk/Memory.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: roma_descriptor_test CHECKPOINT [DUMP_DIRECTORY]\n"); return 2; }
    int result = 0;
    try {
        nn::set_coop_matrix_enabled(false);
        nn::set_log_level(1);
        spirula::roma::Weights weights;
        weights.load(argv[1]);
        std::printf("weights: %llu bytes\n", (unsigned long long)weights.bytes());
        nn::vk::Arena arena("roma-descriptor-test");
        arena.reserve(128ull << 20);
        const int h = 32, w = 48, tokens = h / 16 * (w / 16);
        std::vector<float> rgb((size_t)h * w * 3);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 3; ++c)
                    rgb[((size_t)y * w + x) * 3 + c] = (float)((17 * x + 31 * y + 73 * c) % 256) / 255.0f;
        nn::Tensor input = nn::arena_tensor(arena, nn::DType::F32, h, w, 3);
        nn::tensor_from_host(input, rgb.data(), (int64_t)rgb.size());
        std::array<nn::Tensor, 2> maps{
            nn::arena_tensor(arena, nn::DType::F32, h / 16, w / 16, 1024),
            nn::arena_tensor(arena, nn::DType::F32, h / 16, w / 16, 1024)};
        spirula::roma::descriptor(arena, weights, input, maps);
        for (int tap = 0; tap < 2; ++tap) {
            std::vector<float> host((size_t)tokens * 1024);
            nn::tensor_to_host(maps[tap], host.data(), (int64_t)host.size());
            bool finite = true;
            double sum = 0, sq = 0;
            for (float value : host) { finite = finite && std::isfinite(value); sum += value; sq += (double)value * value; }
            if (!finite || sq == 0) throw nn::Error("descriptor output is nonfinite or empty");
            std::printf("PASS block %d tap: mean %.9g, RMS %.9g\n", tap == 0 ? 11 : 17,
                        sum / host.size(), std::sqrt(sq / host.size()));
            if (argc > 2) {
                const auto root = std::filesystem::path(argv[2]);
                std::filesystem::create_directories(root);
                std::ofstream out(root / ("descriptor_" + std::to_string(tap) + ".f32"), std::ios::binary);
                out.write((const char*)host.data(), (std::streamsize)(host.size() * sizeof(float)));
                if (!out) throw nn::Error("descriptor dump write failed");
            }
        }
    } catch (const std::exception& e) { std::printf("FAIL %s\n", e.what()); result = 1; }
    nn::shutdown();
    return result;
}
