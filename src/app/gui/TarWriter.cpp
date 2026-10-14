// TarWriter.cpp -- see TarWriter.h.

#include "app/gui/TarWriter.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace gui {

namespace {

constexpr size_t kBlock = 512;

void octal(char* field, size_t width, uint64_t value) {
    // width - 1 digits and a NUL; a value too wide for them (a file over
    // 8 GB in an 11-digit size field) takes the base-256 form instead.
    if (value >> (3 * (width - 1))) {
        std::memset(field, 0, width);
        field[0] = (char)0x80;
        for (size_t i = width - 1; i > 0 && value; i--, value >>= 8)
            field[i] = (char)(value & 0xff);
        return;
    }
    std::snprintf(field, width, "%0*llo", (int)(width - 1), (unsigned long long)value);
}

bool header(const TarSink& sink, const std::string& name, uint64_t size, int64_t mtime,
            char type) {
    if (name.size() > 99) {
        // GNU long name: a record named ././@LongLink whose data is the name.
        if (!header(sink, "././@LongLink", name.size() + 1, 0, 'L')) return false;
        std::vector<char> data(((name.size() + 1 + kBlock - 1) / kBlock) * kBlock, 0);
        std::memcpy(data.data(), name.data(), name.size());
        if (!sink(data.data(), data.size())) return false;
    }
    char h[kBlock] = {};
    std::memcpy(h, name.data(), std::min<size_t>(name.size(), 99));
    octal(h + 100, 8, type == '5' ? 0755 : 0644);
    octal(h + 108, 8, 0);
    octal(h + 116, 8, 0);
    octal(h + 124, 12, size);
    octal(h + 136, 12, (uint64_t)(mtime > 0 ? mtime : 0));
    h[156] = type;
    std::memcpy(h + 257, "ustar", 6);
    std::memcpy(h + 263, "00", 2);
    std::memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (size_t i = 0; i < kBlock; i++) sum += (unsigned char)h[i];
    std::snprintf(h + 148, 8, "%06o", sum);
    h[155] = ' ';
    return sink(h, kBlock);
}

int64_t mtime_of(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    return std::chrono::duration_cast<std::chrono::seconds>(
               (t - fs::file_time_type::clock::now() + std::chrono::system_clock::now())
                   .time_since_epoch())
        .count();
}

bool add_file(const TarSink& sink, const fs::path& path, const std::string& name,
              int* skipped) {
    std::ifstream f(path, std::ios::binary);
    std::error_code ec;
    const uint64_t size = fs::file_size(path, ec);
    if (!f || ec) {
        if (skipped) (*skipped)++;
        return true;
    }
    if (!header(sink, name, size, mtime_of(path), '0')) return false;
    std::vector<char> buf(1 << 20);
    uint64_t left = size;
    while (left > 0) {
        const size_t want = (size_t)std::min<uint64_t>(left, buf.size());
        f.read(buf.data(), (std::streamsize)want);
        size_t got = (size_t)f.gcount();
        // A file that shrank while being read is padded to the size the header
        // promised, or every entry after it would be misread.
        if (got < want) std::memset(buf.data() + got, 0, want - got), got = want;
        if (!sink(buf.data(), got)) return false;
        left -= got;
    }
    const size_t pad = (size_t)((kBlock - size % kBlock) % kBlock);
    static const char zeros[kBlock] = {};
    return pad == 0 || sink(zeros, pad);
}

}  // namespace

bool write_tar(const std::string& root_str, const TarSink& sink, int* skipped) {
    const fs::path root = fs::u8path(root_str);
    std::error_code ec;
    const std::string top = root.filename().empty() ? root.parent_path().filename().u8string()
                                                    : root.filename().u8string();
    if (fs::is_regular_file(root, ec)) {
        if (!add_file(sink, root, top, skipped)) return false;
    } else {
        if (!header(sink, top + "/", 0, mtime_of(root), '5')) return false;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec),
            end;
        for (; it != end; it.increment(ec)) {
            if (ec) {
                if (skipped) (*skipped)++;
                ec.clear();
                continue;
            }
            const fs::path rel = fs::relative(it->path(), root, ec);
            if (ec) continue;
            const std::string name = top + "/" + rel.generic_u8string();
            std::error_code ec2;
            if (it->is_directory(ec2)) {
                if (!header(sink, name + "/", 0, mtime_of(it->path()), '5')) return false;
            } else if (it->is_regular_file(ec2)) {
                if (!add_file(sink, it->path(), name, skipped)) return false;
            }
        }
    }
    static const char zeros[2 * kBlock] = {};
    return sink(zeros, sizeof zeros);
}

}  // namespace gui
