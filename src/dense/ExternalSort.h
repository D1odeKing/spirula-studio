#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace spirula::dense {

template<class T> bool read_disk_record(std::istream& input, T& value) {
    input.read(reinterpret_cast<char*>(&value), sizeof(T));
    if (!input.gcount() && input.eof()) return false;
    if (input.gcount() != sizeof(T)) throw std::runtime_error("truncated dense intermediate");
    return true;
}

template<class T> void write_disk_record(std::ostream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(T));
    if (!output) throw std::runtime_error("dense intermediate write failed");
}

inline void flush_disk_output(std::ostream& output) {
    output.flush();
    if (!output) throw std::runtime_error("dense intermediate flush failed");
}

// `progress(done, total)` counts records read into runs and written by each merge
// pass; the total is known up front from the file size and the run capacity.
template<class T, class Compare> void external_sort(const std::filesystem::path& source,
    const std::filesystem::path& destination, uint64_t budget, Compare compare, const std::function<void()>& check = {},
    const std::function<void(uint64_t,uint64_t)>& progress = {}) {
    namespace fs = std::filesystem;
    const uint64_t capacity = std::max<uint64_t>(1, budget / (4 * sizeof(T)));
    std::error_code size_error;
    const uint64_t records = fs::file_size(source, size_error) / sizeof(T);
    uint64_t passes = 0;
    for (uint64_t runs = (records + capacity - 1) / capacity; runs > 1; runs = runs / 2 + runs % 2) ++passes;
    const uint64_t total = std::max<uint64_t>(1, records * (1 + passes));
    uint64_t done = 0;
    auto advance = [&](uint64_t n) {
        const uint64_t before = done; done += n;
        if (progress && (done >> 16) != (before >> 16)) progress(std::min(done, total), total);
    };
    uint64_t run_count = 0;
    auto run_file = [&](uint64_t pass,uint64_t index) -> fs::path {
        return destination.string() + ".sort-" + std::to_string(pass) + "-" + std::to_string(index);
    };
    {
        std::ifstream input(source, std::ios::binary);
        if (!input) throw std::runtime_error("cannot read dense sort input");
        std::vector<T> chunk; chunk.reserve((size_t)capacity); T value;
        for (;;) {
            if (check) check();
            chunk.clear();
            while (chunk.size() < capacity && read_disk_record(input, value)) chunk.push_back(value);
            if (chunk.empty()) break;
            advance(chunk.size());
            std::stable_sort(chunk.begin(), chunk.end(), compare);
            const auto path = run_file(0,run_count++);
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            for (const auto& record : chunk) write_disk_record(output, record);
            flush_disk_output(output);
        }
    }
    uint64_t pass = 0;
    while (run_count > 1) {
        for (uint64_t i = 0; i < run_count; i += 2) {
            if (check) check();
            const auto first = run_file(pass,i), second = run_file(pass,i + 1), path = run_file(pass + 1,i / 2);
            if (i + 1 == run_count) {
                advance(fs::file_size(first, size_error) / sizeof(T));
                fs::rename(first,path); continue;
            }
            {
                std::ifstream a(first, std::ios::binary), b(second, std::ios::binary);
                std::ofstream output(path, std::ios::binary | std::ios::trunc);
                T va, vb; bool has_a = read_disk_record(a, va), has_b = read_disk_record(b, vb);
                while (has_a || has_b) {
                    if (check) check();
                    advance(1);
                    if (has_a && (!has_b || !compare(vb, va))) {
                        write_disk_record(output, va); has_a = read_disk_record(a, va);
                    } else { write_disk_record(output, vb); has_b = read_disk_record(b, vb); }
                }
                flush_disk_output(output);
            }
            fs::remove(first); fs::remove(second);
        }
        run_count = run_count / 2 + run_count % 2; ++pass;
    }
    if (!run_count) { std::ofstream output(destination, std::ios::binary | std::ios::trunc); flush_disk_output(output); }
    else fs::rename(run_file(pass,0),destination);
    if (progress) progress(total, total);
}

}  // namespace spirula::dense
