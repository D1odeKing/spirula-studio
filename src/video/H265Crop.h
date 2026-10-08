#pragma once

#include "video/BitReader.h"

#include <cstddef>
#include <limits>
#include <string>

namespace video {

// One SPS NAL, including its two-byte header, without an Annex-B start code.
inline bool crop_h265_sps(const uint8_t* nal, size_t size, uint32_t width,
                          uint32_t height, std::vector<uint8_t>& out,
                          std::string& error) {
    auto fail = [&](const char* message) {
        error = message;
        return false;
    };
    if (!nal || size < 3 || size > (std::numeric_limits<size_t>::max)() / 8 ||
        (nal[0] & 0x80) || ((nal[0] >> 1) & 0x3f) != 33 || !(nal[1] & 7))
        return fail("invalid H.265 SPS NAL");
    if (!width || !height || (width & 1) || (height & 1))
        return fail("H.265 display dimensions must be positive and even");

    RbspReader br(nal, size, 2);
    const auto& source = br.rbsp();
    if (source.empty() || !source.back())
        return fail("missing H.265 SPS trailing bits");
    size_t stop = source.size() * 8 - 1;
    while (!((source[stop >> 3] >> (7 - (stop & 7))) & 1)) --stop;
    auto skip = [&](size_t bits) {
        if (br.bitPos() > stop || bits > stop - br.bitPos()) return false;
        br.seekBits(br.bitPos() + bits);
        return true;
    };

    br.u(4);
    const uint32_t max_sub = br.u(3);
    br.bit();
    if (max_sub > 6 || !skip(96))
        return fail("invalid or truncated H.265 SPS profile-tier-level");
    bool sub_profile[6] = {}, sub_level[6] = {};
    for (uint32_t i = 0; i < max_sub; ++i) {
        sub_profile[i] = br.flag();
        sub_level[i] = br.flag();
    }
    if (max_sub && !skip(2 * (8 - max_sub)))
        return fail("truncated H.265 SPS sub-layer flags");
    for (uint32_t i = 0; i < max_sub; ++i) {
        if ((sub_profile[i] && !skip(88)) || (sub_level[i] && !skip(8)))
            return fail("truncated H.265 SPS sub-layer profile-tier-level");
    }
    const uint32_t sps_id = br.ue();
    const uint32_t chroma = br.ue();
    if (br.overrun() || br.bitPos() > stop)
        return fail("truncated H.265 SPS format");
    if (sps_id > 15 || chroma != 1)
        return fail("H.265 SPS crop requires a 4:2:0 SPS");
    const uint32_t coded_width = br.ue();
    const uint32_t coded_height = br.ue();
    const size_t crop_begin = br.bitPos();
    uint32_t left = 0, right = 0, top = 0, bottom = 0;
    if (br.flag()) {
        left = br.ue();
        right = br.ue();
        top = br.ue();
        bottom = br.ue();
    }
    const size_t crop_end = br.bitPos();
    if (br.overrun() || crop_end >= stop)
        return fail("truncated H.265 SPS conformance window");
    if (!coded_width || !coded_height ||
        2 * (uint64_t(left) + right) >= coded_width ||
        2 * (uint64_t(top) + bottom) >= coded_height)
        return fail("invalid H.265 SPS conformance window");
    const uint64_t used_width = uint64_t(width) + 2 * uint64_t(left);
    const uint64_t used_height = uint64_t(height) + 2 * uint64_t(top);
    if (used_width > coded_width || used_height > coded_height ||
        ((coded_width - used_width) & 1) || ((coded_height - used_height) & 1))
        return fail("H.265 display dimensions exceed the coded crop extent");
    right = uint32_t((coded_width - used_width) / 2);
    bottom = uint32_t((coded_height - used_height) / 2);

    std::vector<uint8_t> rewritten;
    rewritten.reserve(source.size() + 32);
    size_t pos = 0;
    auto bit = [&](uint32_t value) {
        if (!(pos & 7)) rewritten.push_back(0);
        rewritten.back() |= uint8_t((value & 1) << (7 - (pos & 7)));
        ++pos;
    };
    auto copy = [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i)
            bit(source[i >> 3] >> (7 - (i & 7)));
    };
    auto ue = [&](uint32_t value) {
        const uint64_t code = uint64_t(value) + 1;
        int bits = 0;
        for (uint64_t v = code; v; v >>= 1) ++bits;
        for (int i = 1; i < bits; ++i) bit(0);
        for (int i = bits - 1; i >= 0; --i) bit(uint32_t(code >> i));
    };
    copy(0, crop_begin);
    const bool cropped = left || right || top || bottom;
    bit(cropped);
    if (cropped) {
        ue(left);
        ue(right);
        ue(top);
        ue(bottom);
    }
    copy(crop_end, stop + 1);
    while (pos & 7) bit(0);

    std::vector<uint8_t> escaped;
    escaped.reserve(2 + rewritten.size() + rewritten.size() / 2);
    escaped.push_back(nal[0]);
    escaped.push_back(nal[1]);
    int zeros = 0;
    for (uint8_t byte : rewritten) {
        if (zeros == 2 && byte <= 3) {
            escaped.push_back(3);
            zeros = 0;
        }
        escaped.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    out.swap(escaped);
    error.clear();
    return true;
}

}  // namespace video
