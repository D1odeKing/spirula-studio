#pragma once
// How much memory the machine has, or 0 when the platform will not say.
// Callers that need a working number must supply their own fallback.
// On Linux a memory cgroup (a container's limit) caps both answers: sysconf and
// /proc/meminfo describe the host, so a 117 GB pod on a 945 GB host saw 945 GB.

#include <cstddef>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN   // rpcndr.h's `#define small char` reaches far from here
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <cstdio>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <algorithm>
#include <cstring>
#include <string>
#endif

namespace spirula {

#if defined(__linux__)
// The tightest memory cgroup over this process: its limit, and what that limit still leaves
// (usage minus inactive page cache, which the kernel reclaims first). limit 0: no cgroup limit.
struct CgroupMemory {
    size_t limit = 0;
    size_t room = 0;
};

namespace host_memory_detail {

// 0 for a missing file and for v2's "max".
inline unsigned long long read_number(const std::string& path) {
    unsigned long long v = 0;
    if (FILE* f = std::fopen(path.c_str(), "r")) {
        if (std::fscanf(f, "%llu", &v) != 1) v = 0;
        std::fclose(f);
    }
    return v;
}

inline unsigned long long stat_field(const std::string& path, const char* key) {
    unsigned long long v = 0, x = 0;
    char name[64];
    if (FILE* f = std::fopen(path.c_str(), "r")) {
        while (std::fscanf(f, "%63s %llu", name, &x) == 2)
            if (!std::strcmp(name, key)) { v = x; break; }
        std::fclose(f);
    }
    return v;
}

inline bool exists(const std::string& path) { return access(path.c_str(), F_OK) == 0; }

inline void take(CgroupMemory& out, unsigned long long limit, unsigned long long used) {
    if (!limit || limit >= (1ull << 60)) return;   // v1 writes "unlimited" as ~2^63
    const size_t room = used < limit ? (size_t)(limit - used) : 0;
    if (!out.limit || limit < out.limit) out.limit = (size_t)limit;
    if (!out.room || room < out.room) out.room = room;
}

}  // namespace host_memory_detail

// Paths are parameters for the test; a container mounts its own cgroup at the root, so a
// /proc/self/cgroup path that does not exist under `root` falls back to the root itself.
inline CgroupMemory cgroupMemory(const char* proc_cgroup = "/proc/self/cgroup", const char* root = "/sys/fs/cgroup") {
    namespace d = host_memory_detail;
    CgroupMemory out;
    std::string v2, v1;
    bool has_v2 = false, has_v1 = false;
    if (FILE* f = std::fopen(proc_cgroup, "r")) {
        char line[1024];
        while (std::fgets(line, sizeof line, f)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (s.rfind("0::", 0) == 0) { v2 = s.substr(3); has_v2 = true; }
            const size_t c = s.find(':'), c2 = c == std::string::npos ? c : s.find(':', c + 1);
            if (c2 != std::string::npos) {
                const std::string controllers = "," + s.substr(c + 1, c2 - c - 1) + ",";
                if (controllers.find(",memory,") != std::string::npos) { v1 = s.substr(c2 + 1); has_v1 = true; }
            }
        }
        std::fclose(f);
    }
    const std::string base = root;
    if (has_v1 || d::exists(base + "/memory/memory.limit_in_bytes")) {
        std::string dir = base + "/memory" + (v1 == "/" ? "" : v1);
        if (!d::exists(dir + "/memory.limit_in_bytes")) dir = base + "/memory";
        // hierarchical_memory_limit already folds in every ancestor's limit.
        unsigned long long limit = d::stat_field(dir + "/memory.stat", "hierarchical_memory_limit");
        if (!limit) limit = d::read_number(dir + "/memory.limit_in_bytes");
        const unsigned long long usage = d::read_number(dir + "/memory.usage_in_bytes");
        const unsigned long long cache = d::stat_field(dir + "/memory.stat", "total_inactive_file");
        d::take(out, limit, usage > cache ? usage - cache : 0);
    }
    if (has_v2 || d::exists(base + "/memory.max")) {
        std::string dir = base + (v2 == "/" ? "" : v2);
        if (!d::exists(dir + "/memory.current")) dir = base;
        for (;;) {   // an ancestor's limit can be the tighter one
            const unsigned long long usage = d::read_number(dir + "/memory.current");
            const unsigned long long cache = d::stat_field(dir + "/memory.stat", "inactive_file");
            d::take(out, d::read_number(dir + "/memory.max"), usage > cache ? usage - cache : 0);
            if (dir.size() <= base.size()) break;
            dir = dir.substr(0, dir.rfind('/'));
        }
    }
    return out;
}
#endif

inline size_t physicalRamBytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX st{};
    st.dwLength = sizeof(st);
    if (GlobalMemoryStatusEx(&st)) return (size_t)st.ullTotalPhys;
#elif defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
    long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page > 0) {
        size_t host = (size_t)pages * (size_t)page;
#if defined(__linux__)
        if (const size_t limit = cgroupMemory().limit) host = std::min(host, limit);
#endif
        return host;
    }
#endif
    return 0;
}

inline size_t availableRamBytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX st{};
    st.dwLength = sizeof(st);
    if (GlobalMemoryStatusEx(&st)) return (size_t)st.ullAvailPhys;
#elif defined(__linux__)
    const CgroupMemory cgroup = cgroupMemory();
    // Not sysconf(_SC_AVPHYS_PAGES): glibc answers MemFree, which leaves out the page cache.
    if (FILE* file = std::fopen("/proc/meminfo", "r")) {
        char line[128];
        unsigned long long kb = 0;
        bool found = false;
        while (!found && std::fgets(line, sizeof line, file))
            found = std::sscanf(line, "MemAvailable: %llu kB", &kb) == 1;
        std::fclose(file);
        if (found) return cgroup.limit ? std::min((size_t)kb * 1024, cgroup.room) : (size_t)kb * 1024;
    }
    if (cgroup.limit) return cgroup.room;
#elif defined(_SC_AVPHYS_PAGES) && defined(_SC_PAGESIZE)
    long pages = sysconf(_SC_AVPHYS_PAGES), page = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page > 0) return (size_t)pages * (size_t)page;
#endif
    return 0;
}

// What new allocations can still take: Windows refuses them at the commit limit, which
// a small page file puts well below free RAM, and charges Vulkan device memory to it too.
inline size_t availableCommitBytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX st{};
    st.dwLength = sizeof(st);
    if (GlobalMemoryStatusEx(&st)) return (size_t)st.ullAvailPageFile;
    return 0;
#else
    return availableRamBytes();
#endif
}

// This process's resident set: the physical memory it holds right now.
inline size_t processRamBytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters)) return (size_t)counters.WorkingSetSize;
#elif defined(__linux__)
    if (FILE* file = std::fopen("/proc/self/statm", "r")) {
        unsigned long size = 0, resident = 0;
        const int read = std::fscanf(file, "%lu %lu", &size, &resident);
        std::fclose(file);
        const long page = sysconf(_SC_PAGESIZE);
        if (read == 2 && page > 0) return (size_t)resident * (size_t)page;
    }
#endif
    return 0;
}

}  // namespace spirula
