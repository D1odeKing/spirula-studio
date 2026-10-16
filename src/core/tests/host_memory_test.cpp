// host_memory_test -- the memory cgroup cap in core/HostMemory.h, on fake cgroup v1 and v2
// trees: a container's limit and room, an ancestor tighter than the leaf, "max" and v1's
// unlimited sentinel, and a /proc/self/cgroup path missing under the root. Linux only.

#include "core/HostMemory.h"

#include <cstdio>
#include <string>

#if defined(__linux__)
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_failures++;
}

void put(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
}

constexpr unsigned long long GB = 1000ull * 1000 * 1000;

}  // namespace

int main() {
    const fs::path tmp = fs::temp_directory_path() / ("host_memory_test_" + std::to_string(getpid()));
    fs::remove_all(tmp);

    {   // v2 inside a container: its own cgroup mounted at the root
        const fs::path root = tmp / "v2" / "cg", proc = tmp / "v2" / "cgroup";
        put(proc, "0::/\n");
        put(root / "memory.max", std::to_string(117 * GB) + "\n");
        put(root / "memory.current", std::to_string(47 * GB) + "\n");
        put(root / "memory.stat", "anon 30000000000\ninactive_file " + std::to_string(7 * GB) + "\n");
        const auto m = spirula::cgroupMemory(proc.c_str(), root.c_str());
        check(m.limit == 117 * GB, "v2: the container's memory.max is the limit");
        check(m.room == 77 * GB, "v2: room is the limit less usage, inactive page cache not counted");
    }
    {   // v2 on a host: a leaf with no limit under a slice that has one
        const fs::path root = tmp / "v2h" / "cg", proc = tmp / "v2h" / "cgroup";
        put(proc, "0::/work.slice/job.scope\n");
        put(root / "work.slice" / "memory.max", std::to_string(32 * GB) + "\n");
        put(root / "work.slice" / "memory.current", std::to_string(30 * GB) + "\n");
        put(root / "work.slice" / "job.scope" / "memory.max", "max\n");
        put(root / "work.slice" / "job.scope" / "memory.current", std::to_string(10 * GB) + "\n");
        const auto m = spirula::cgroupMemory(proc.c_str(), root.c_str());
        check(m.limit == 32 * GB, "v2: an ancestor's limit applies when the leaf says max");
        check(m.room == 2 * GB, "v2: room is measured where the limit is, not at the leaf");
    }
    {   // v2 with no limit anywhere
        const fs::path root = tmp / "v2n" / "cg", proc = tmp / "v2n" / "cgroup";
        put(proc, "0::/\n");
        put(root / "memory.max", "max\n");
        put(root / "memory.current", std::to_string(5 * GB) + "\n");
        check(spirula::cgroupMemory(proc.c_str(), root.c_str()).limit == 0, "v2: max is no limit");
    }
    {   // v1, a docker path that is not under the container's own mount
        const fs::path root = tmp / "v1" / "cg", proc = tmp / "v1" / "cgroup";
        put(proc, "12:cpu,cpuacct:/docker/abc\n4:memory:/docker/abc\n");
        put(root / "memory" / "memory.limit_in_bytes", std::to_string(64 * GB) + "\n");
        put(root / "memory" / "memory.usage_in_bytes", std::to_string(20 * GB) + "\n");
        put(root / "memory" / "memory.stat",
            "hierarchical_memory_limit " + std::to_string(48 * GB) + "\ntotal_inactive_file " +
                std::to_string(4 * GB) + "\n");
        const auto m = spirula::cgroupMemory(proc.c_str(), root.c_str());
        check(m.limit == 48 * GB, "v1: hierarchical_memory_limit wins over the leaf's own limit");
        check(m.room == 32 * GB, "v1: room leaves out inactive page cache");
    }
    {   // v1 unlimited
        const fs::path root = tmp / "v1u" / "cg", proc = tmp / "v1u" / "cgroup";
        put(proc, "4:memory:/\n");
        put(root / "memory" / "memory.limit_in_bytes", "9223372036854771712\n");
        put(root / "memory" / "memory.usage_in_bytes", std::to_string(3 * GB) + "\n");
        check(spirula::cgroupMemory(proc.c_str(), root.c_str()).limit == 0, "v1: the unlimited sentinel is no limit");
    }
    {   // no cgroup files at all
        const fs::path root = tmp / "none" / "cg", proc = tmp / "none" / "cgroup";
        check(spirula::cgroupMemory(proc.c_str(), root.c_str()).limit == 0, "nothing to read is no limit");
    }
    {
        const size_t total = spirula::physicalRamBytes(), avail = spirula::availableRamBytes();
        const auto here = spirula::cgroupMemory();
        check(total > 0 && avail > 0 && avail <= total, "this machine: available fits in physical");
        check(!here.limit || total <= here.limit, "this machine: physical is capped by its own cgroup");
        std::printf("     this machine: physical %.1f GB, available %.1f GB, cgroup limit %.1f GB\n",
                    total / 1e9, avail / 1e9, here.limit / 1e9);
    }

    fs::remove_all(tmp);
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}

#else
int main() {
    std::printf("host_memory_test: Linux only (cgroups); nothing to check here\n");
    return 0;
}
#endif
