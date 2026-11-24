#pragma once

// A range split across std::threads, for the mesh-lab tools: the build does
// not promise OpenMP on every compiler, and these loops are coarse.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace meshing {

inline void parallel_for(int64_t n, int threads, const std::function<void(int64_t, int64_t)>& range) {
    if (n <= 0) return;
    int t = threads > 0 ? threads : (int)std::max(1u, std::thread::hardware_concurrency());
    t = (int)std::min<int64_t>(t, std::max<int64_t>(1, n / 256));
    if (t <= 1) { range(0, n); return; }
    std::vector<std::thread> pool;
    const int64_t chunk = (n + t - 1) / t;
    for (int i = 0; i < t; ++i) {
        const int64_t b = i * chunk, e = std::min(n, b + chunk);
        if (b < e) pool.emplace_back([&range, b, e] { range(b, e); });
    }
    for (auto& th : pool) th.join();
}

}  // namespace meshing
