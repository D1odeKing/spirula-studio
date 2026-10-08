#include "video/H265Crop.h"

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fixture {
    std::vector<uint8_t> nal;
    std::vector<uint8_t> prefix;
    std::vector<uint8_t> suffix;
};

Fixture make_sps(uint32_t layers, bool cropped, uint32_t left = 0,
                 uint32_t top = 0, uint32_t chroma = 1,
                 uint32_t coded_width = 384, uint32_t right = 4) {
    std::vector<uint8_t> bits;
    auto u = [&](uint64_t value, int count) {
        for (int i = count - 1; i >= 0; --i)
            bits.push_back(uint8_t((value >> i) & 1));
    };
    auto ue = [&](uint32_t value) {
        const uint64_t code = uint64_t(value) + 1;
        int count = 0;
        for (uint64_t v = code; v; v >>= 1) ++count;
        u(0, count - 1);
        u(code, count);
    };
    auto profile = [&](uint32_t seed) {
        u(seed & 3, 2);
        u(seed & 1, 1);
        u(1, 5);
        u(0x40000000, 32);
        u(seed & 15, 4);
        u(0, 44);
    };
    u(3, 4);
    u(layers, 3);
    u(1, 1);
    profile(layers);
    u(120 + layers, 8);
    for (uint32_t i = 0; i < layers; ++i) {
        u(i & 1, 1);
        u((i >> 1) & 1, 1);
    }
    if (layers) u(0, 2 * (8 - layers));
    for (uint32_t i = 0; i < layers; ++i) {
        if (i & 1) profile(i + 1);
        if ((i >> 1) & 1) u(90 + i, 8);
    }
    ue(2);
    ue(chroma);
    if (chroma == 3) u(1, 1);
    ue(coded_width);
    ue(192);
    Fixture fixture;
    fixture.prefix = bits;
    u(cropped, 1);
    if (cropped) {
        ue(left);
        ue(right);
        ue(top);
        ue(5);
    }
    const size_t suffix_begin = bits.size();
    ue(0);
    ue(0);
    ue(4);
    u(0, 48);
    u(0x5a71, 16);
    u(1, 1);
    fixture.suffix.assign(bits.begin() + suffix_begin, bits.end());
    while (bits.size() & 7) u(0, 1);
    fixture.nal = {0x42, 0x01};
    int zeros = 0;
    for (size_t i = 0; i < bits.size(); i += 8) {
        uint8_t byte = 0;
        for (size_t j = 0; j < 8; ++j) byte = uint8_t((byte << 1) | bits[i + j]);
        if (zeros == 2 && byte <= 3) {
            fixture.nal.push_back(3);
            zeros = 0;
        }
        fixture.nal.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    return fixture;
}

void check_crop(const Fixture& fixture, uint32_t width, uint32_t height,
                uint32_t left, uint32_t top) {
    std::vector<uint8_t> out = {0xaa};
    std::string error = "stale error";
    if (!video::crop_h265_sps(fixture.nal.data(), fixture.nal.size(),
                             width, height, out, error))
        throw std::runtime_error(error);
    require(error.empty(), "success did not clear error");
    require(out[0] == fixture.nal[0] && out[1] == fixture.nal[1], "NAL header changed");
    bool escaped = false;
    for (size_t i = 4; i < out.size(); ++i)
        escaped |= out[i - 2] == 0 && out[i - 1] == 0 && out[i] == 3;
    require(escaped, "fixture did not exercise emulation prevention");

    video::RbspReader br(out.data(), out.size(), 2);
    for (uint8_t bit : fixture.prefix)
        require(br.bit() == bit, "SPS prefix or coded dimensions changed");
    br.seekBits(4);
    const uint32_t layers = br.u(3);
    br.bit();
    br.seekBits(br.bitPos() + 96);
    bool profiles[6] = {}, levels[6] = {};
    for (uint32_t i = 0; i < layers; ++i) {
        profiles[i] = br.flag();
        levels[i] = br.flag();
    }
    if (layers) br.seekBits(br.bitPos() + 2 * (8 - layers));
    for (uint32_t i = 0; i < layers; ++i)
        br.seekBits(br.bitPos() + (profiles[i] ? 88 : 0) + (levels[i] ? 8 : 0));
    require(br.ue() == 2 && br.ue() == 1, "SPS id or chroma changed");
    const uint32_t coded_width = br.ue(), coded_height = br.ue();
    require(coded_width == 384 && coded_height == 192, "coded extent changed");
    uint32_t l = 0, r = 0, t = 0, b = 0;
    const bool cropped = br.flag();
    if (cropped) {
        l = br.ue();
        r = br.ue();
        t = br.ue();
        b = br.ue();
    }
    require(l == left && t == top, "driver left/top crop changed");
    require(coded_width - 2 * (l + r) == width &&
            coded_height - 2 * (t + b) == height, "incorrect visible extent");
    require(cropped == (l || r || t || b), "incorrect conformance flag");
    for (uint8_t bit : fixture.suffix)
        require(br.bit() == bit, "SPS suffix or stop bit changed");
    require(br.sizeBits() - br.bitPos() < 8, "extra trailing bytes");
    while (br.bitPos() < br.sizeBits()) require(br.bit() == 0, "nonzero alignment bit");
    require(!br.overrun(), "rewritten SPS was truncated");
    std::vector<uint8_t> repeated;
    require(video::crop_h265_sps(out.data(), out.size(), width, height, repeated, error),
            "idempotent crop failed");
    require(repeated == out, "crop is not idempotent");
    require(video::crop_h265_sps(out.data(), out.size(), width, height, out, error),
            "in-place crop failed");
    require(repeated == out, "in-place crop changed the result");
}

void reject(const std::vector<uint8_t>& nal, uint32_t width, uint32_t height) {
    const std::vector<uint8_t> sentinel = {0xaa, 0xbb};
    auto out = sentinel;
    std::string error;
    require(!video::crop_h265_sps(nal.data(), nal.size(), width, height, out, error),
            "invalid SPS crop accepted");
    require(out == sentinel && !error.empty(), "failure changed output or omitted error");
}

}  // namespace

int main() {
    try {
        for (uint32_t layers = 0; layers <= 6; ++layers) {
            check_crop(make_sps(layers, false), 256, 160, 0, 0);
            check_crop(make_sps(layers, true, 2, 3), 256, 160, 2, 3);
        }
        check_crop(make_sps(0, false), 384, 192, 0, 0);
        check_crop(make_sps(3, true), 384, 192, 0, 0);
        const auto fixture = make_sps(0, false);
        reject(fixture.nal, 386, 160);
        reject(fixture.nal, 256, 194);
        reject(fixture.nal, UINT32_MAX - 1, 160);
        reject(fixture.nal, 255, 160);
        reject(fixture.nal, 256, 159);
        reject(fixture.nal, 0, 160);
        reject(make_sps(0, true, 2, 3).nal, 384, 192);
        reject(make_sps(0, true, UINT32_MAX).nal, 256, 160);
        reject(make_sps(0, true, 0, UINT32_MAX).nal, 256, 160);
        reject(make_sps(0, true, 0, 0, 1, 384, UINT32_MAX).nal, 256, 160);
        reject(make_sps(0, false, 0, 0, 1, 383).nal, 256, 160);
        for (uint32_t chroma : {0u, 2u, 3u})
            reject(make_sps(0, false, 0, 0, chroma).nal, 256, 160);
        reject(make_sps(7, false).nal, 256, 160);
        for (size_t length = 0; length <= 15; ++length)
            reject(std::vector<uint8_t>(fixture.nal.begin(), fixture.nal.begin() + length), 256, 160);
        auto invalid = fixture.nal;
        invalid[0] = 0x40;
        reject(invalid, 256, 160);
        invalid[0] = 0xc2;
        reject(invalid, 256, 160);
        invalid[0] = 0x42;
        invalid[1] = 0;
        reject(invalid, 256, 160);
        invalid = fixture.nal;
        invalid.back() = 0;
        reject(invalid, 256, 160);
        std::cout << "HEVC SPS crop CPU regression passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
