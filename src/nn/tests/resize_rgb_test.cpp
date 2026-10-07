#include "nn/io/Resize.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    try {
        const int w = 1280, h = 960;
        for (bool aliasing : {false, true}) {
            std::vector<float> input((size_t)w * h * 3);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    for (int c = 0; c < 3; ++c)
                        input[((size_t)y * w + x) * 3 + c] =
                            (float)((17 * (aliasing ? x : x / 8) + 31 * (aliasing ? y : y / 8) + 73 * c) % 256) / 255;
            for (const auto& size : {std::array<int, 2>{320, 320}, {512, 512}, {640, 640}, {800, 800}, {1280, 1280}, {48, 32}}) {
                const auto resized = nn::resize_rgb_bicubic(input.data(), w, h, size[0], size[1], nn::BicubicWeights::Float32);
                for (float value : resized) if (!std::isfinite(value)) throw std::runtime_error("nonfinite resize output");
                const std::string name = std::string(aliasing ? "aliasing_" : "textured_") +
                    std::to_string(size[0]) + "x" + std::to_string(size[1]);
                if (argc > 1) {
                    std::filesystem::create_directories(argv[1]);
                    std::ofstream out(std::filesystem::path(argv[1]) / (name + ".f32"), std::ios::binary);
                    out.write((const char*)resized.data(), (std::streamsize)(resized.size() * sizeof(float)));
                    if (!out) throw std::runtime_error("resize dump write failed");
                }
                std::printf("PASS %s finite output\n", name.c_str());
            }
            const auto unchanged = nn::resize_rgb_bicubic(input.data(), w, h, w, h, nn::BicubicWeights::Float32);
            if (unchanged != input) throw std::runtime_error("identity resize changed pixels");
        }
        const float constant[] = {0.2f, 0.4f, 0.8f};
        for (auto mode : {nn::BicubicWeights::Float32, nn::BicubicWeights::Pillow}) {
            const auto out = nn::resize_rgb_bicubic(constant, 1, 1, 19, 23, mode);
            for (size_t i = 0; i < out.size(); ++i)
                if (std::abs(out[i] - constant[i % 3]) > 5e-7f) throw std::runtime_error("constant resize changed color");
        }
        return 0;
    } catch (const std::exception& e) { std::printf("FAIL %s\n", e.what()); return 1; }
}
