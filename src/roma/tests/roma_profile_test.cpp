#include "roma/Roma.h"

#include "core/Env.h"
#include "nn/Device.h"
#include "nn/io/Image.h"
#include "nn/core/Log.h"
#include "nn/vk/Stream.h"
#include "nn/vk/Context.h"
#include "nn/vk/Memory.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: roma_profile_test CHECKPOINT [PRESET] [DUMP_DIRECTORY] [textured|aliasing|IMAGE_A IMAGE_B]\n"); return 2; }
    int result = 0;
    try {
        using Clock = std::chrono::steady_clock;
        using namespace spirula::roma;
        nn::set_log_level(1);
        Session session;
        const auto start = Clock::now();
        const char* precision_text = spirula::env("ROMA_PROFILE_PRECISION");
        const std::string precision = precision_text ? precision_text : "float32";
        if (precision != "auto" && precision != "float32" && precision != "mixed")
            throw std::runtime_error("SS_ROMA_PROFILE_PRECISION must be auto, float32, or mixed");
        session.load(argv[1], precision == "auto" ? InferencePrecision::Automatic :
                     precision == "mixed" ? InferencePrecision::Mixed : InferencePrecision::Float32);
        std::printf("Precision: %s\n", session.precision() == InferencePrecision::Mixed ? "mixed" : "float32");
        const auto& device = nn::vk::Context::get();
        std::printf("Cooperative matrix: %s\n", device.hasCoopMat() ? "enabled" : device.coopMatReason().c_str());
        std::printf("Load: %.3f seconds; weights: %llu bytes\n",
                    std::chrono::duration<double>(Clock::now() - start).count(),
                    (unsigned long long)session.deviceBytes());
        int aw = 1280, ah = 960, bw = aw, bh = ah;
        const bool aliasing = argc > 4 && std::string(argv[4]) == "aliasing";
        std::vector<float> a, b;
        if (argc > 5) {
            auto decode = [](const char* path, int& width, int& height) {
                const auto image = nn::load_image(path);
                if (image.empty()) throw std::runtime_error("profile image decode failed");
                width = image.width; height = image.height;
                std::vector<float> rgb(image.data.size());
                std::transform(image.data.begin(),image.data.end(),rgb.begin(),[](uint8_t value) { return value / 255.0f; });
                return rgb;
            };
            a = decode(argv[4],aw,ah); b = decode(argv[5],bw,bh);
        } else {
            if (argc > 4 && !aliasing && std::string(argv[4]) != "textured")
                throw std::runtime_error("profile pattern must be textured, aliasing, or two image paths");
            a.resize((size_t)aw * ah * 3); b.resize(a.size());
            for (int y = 0; y < ah; ++y)
                for (int x = 0; x < aw; ++x)
                for (int c = 0; c < 3; ++c) {
                    const size_t i = ((size_t)y * aw + x) * 3 + c;
                    const int xa = aliasing ? x : x / 8, xb = aliasing ? x + 11 : (x + 11) / 8;
                    const int yy = aliasing ? y : y / 8;
                    a[i] = (float)((17 * xa + 31 * yy + 73 * c) % 256) / 255;
                    b[i] = (float)((17 * xb + 31 * yy + 73 * c) % 256) / 255;
                }
        }
        auto options = MatchOptions::preset(argc > 2 ? argv[2] : "precise");
        auto last = Clock::now();
        options.progress = [&](const char* stage) {
            const auto now = Clock::now();
            std::printf("%s: %.3f seconds since previous stage\n", stage, std::chrono::duration<double>(now - last).count());
            std::fflush(stdout);
            last = now;
        };
        const char* repeat_text = spirula::env("ROMA_PROFILE_REPEAT");
        const int repeats = std::max(1, repeat_text ? std::atoi(repeat_text) : 1);
        const char* cache_text = spirula::env("ROMA_PROFILE_CACHE");
        const std::string cache_mode = cache_text ? cache_text : "off";
        if (cache_mode != "off" && cache_mode != "both" && cache_mode != "reference")
            throw std::runtime_error("SS_ROMA_PROFILE_CACHE must be off, both, or reference");
        std::printf("Feature cache: %s\n", cache_mode.c_str());
        PairPrediction prediction;
        auto match_start = Clock::now();
        for (int r = 0; r < repeats; ++r) {
            match_start = Clock::now();
            last = match_start;
            if (cache_mode == "off") prediction = session.match(a.data(), aw, ah, b.data(), bw, bh, options);
            else prediction = session.matchCached(0, a.data(), aw, ah, cache_mode == "reference" ? r + 1 : 1,
                                                    b.data(), bw, bh, options);
            std::printf("Match %d: %.3f seconds\n", r + 1, std::chrono::duration<double>(Clock::now() - match_start).count());
        }
        nn::set_log_level(2);
        nn::vk::Stream::get().report();
        const auto cache = session.featureCacheStatistics();
        std::printf("Feature cache hits %llu; misses %llu; evictions %llu; held %llu; peak %llu; limit %llu bytes\n",
                    (unsigned long long)cache.hits, (unsigned long long)cache.misses, (unsigned long long)cache.evictions,
                    (unsigned long long)cache.bytes, (unsigned long long)cache.peak_bytes, (unsigned long long)cache.limit_bytes);
        std::printf("PASS %dx%d; directions %d; %.3f seconds; planned scratch %llu; peak scratch %llu; held device bytes %llu\n",
                    prediction.forward.width, prediction.forward.height, prediction.backward.warp.empty() ? 1 : 2,
                    std::chrono::duration<double>(Clock::now() - match_start).count(),
                    (unsigned long long)Session::plannedScratchBytes(options),
                    (unsigned long long)session.peakScratchBytes(), (unsigned long long)session.deviceBytes());
        std::printf("Peak allocated Vulkan buffers (including host-visible staging): %llu bytes\n",
                    (unsigned long long)nn::vk::Allocator::get().peakBytes());
        if (argc > 3) {
            const std::filesystem::path root(argv[3]);
            std::filesystem::create_directories(root);
            auto dump = [&](const std::vector<float>& values, const std::string& name) {
                std::ofstream out(root / (name + ".f32"), std::ios::binary);
                out.write((const char*)values.data(), (std::streamsize)(values.size() * sizeof(float)));
                if (!out) throw std::runtime_error("profile dump write failed");
            };
            dump(a,"input_a"); dump(b,"input_b");
            {
                std::ofstream metadata(root / "input_dimensions.txt");
                metadata << aw << ' ' << ah << ' ' << bw << ' ' << bh << '\n';
                if (!metadata) throw std::runtime_error("profile input metadata write failed");
            }
            for (const auto& direction : {std::make_pair(&prediction.forward, "AB"), std::make_pair(&prediction.backward, "BA")}) {
                if (direction.first->warp.empty()) continue;
                const std::string prefix = std::string("profile_") + direction.second + "_";
                dump(direction.first->warp, prefix + "warp");
                dump(direction.first->overlap, prefix + "overlap");
                dump(direction.first->precision, prefix + "precision");
            }
        }
    } catch (const std::exception& e) { std::printf("FAIL %s\n", e.what()); result = 1; }
    nn::shutdown();
    return result;
}
