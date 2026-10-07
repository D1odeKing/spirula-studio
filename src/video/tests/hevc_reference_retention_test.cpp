#include "video/VideoPipeline.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main() {
    try {
        const fs::path fixtures = fs::path(SS_SOURCE_ROOT) / "src/video/tests/data";
        constexpr size_t frame_bytes = 320 * 180 * 3;
        for (const std::string name : {"hevc_main_retention", "hevc_main10_retention"}) {
            const fs::path video_path = fixtures / (name + ".mp4");
            std::ifstream reference(fixtures / (name + ".rgb"), std::ios::binary);
            if (!reference) throw std::runtime_error("cannot open software-decoded RGB reference");
            const std::vector<uint8_t> expected((std::istreambuf_iterator<char>(reference)),
                                                 std::istreambuf_iterator<char>());
            if (expected.size() != 2 * frame_bytes)
                throw std::runtime_error("invalid software reference size");

            std::string error;
            video::VideoPipeline decoder;
            if (!decoder.open(video_path.string(), 0, 0, error))
                throw std::runtime_error(error);
            if (decoder.format().bit_depth != (name == "hevc_main_retention" ? 8 : 10))
                throw std::runtime_error("unexpected HEVC profile");

            auto compare = [&](const video::FrameHandle& frame) {
                nn::Image image;
                if (!decoder.toImage(frame, {}, image, error))
                    throw std::runtime_error(error);
                if (image.width != 320 || image.height != 180 || image.data.size() != frame_bytes)
                    throw std::runtime_error("unexpected decoded image dimensions");
                const uint8_t* want = expected.data() + (frame.index == 22 ? 0 : frame_bytes);
                double sum = 0;
                for (size_t i = 0; i < frame_bytes; ++i) {
                    const double delta = double(image.data[i]) - double(want[i]);
                    sum += delta * delta;
                }
                const double psnr = 10.0 * std::log10(255.0 * 255.0 * frame_bytes / sum);
                std::cout << name << " frame " << frame.index << " software RGB PSNR: "
                          << psnr << " dB\n";
                if (psnr < 28.0) throw std::runtime_error("HEVC reference picture corruption");
            };

            int decoded = 0;
            int compared = 0;
            video::FrameHandle frame;
            while (decoder.next(frame, error)) {
                if (frame.index == 22 || frame.index == 46) {
                    compare(frame);
                    ++compared;
                }
                decoder.release(frame);
                ++decoded;
            }
            if (!error.empty()) throw std::runtime_error(error);
            if (decoded != 72 || compared != 2)
                throw std::runtime_error("HEVC fixture did not exercise both retained-reference GOPs");

            for (int target : {46, 22}) {
                int64_t landed = -1;
                if (!decoder.seek(target, landed, error) || landed < 0 || landed > target)
                    throw std::runtime_error("HEVC seek failed: " + error);
                bool found = false;
                while (decoder.next(frame, error)) {
                    if (frame.index == target) {
                        compare(frame);
                        found = true;
                    }
                    decoder.release(frame);
                    if (found || frame.index > target) break;
                }
                if (!error.empty() || !found)
                    throw std::runtime_error("HEVC seek did not reproduce the reference picture");
            }
            std::cout << name << ": 72 frames and two backward-seek pixel comparisons passed\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
