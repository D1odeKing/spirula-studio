#include "dense/MemoryReport.h"

#include <cstdio>
#include <filesystem>
#include <stdexcept>

namespace {

using namespace spirula::dense;

void check(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}

}  // namespace

int main() {
    try {
        constexpr uint64_t GiB = 1ull << 30;
        check(planned_host_bytes(32 * GiB, false) == 32 * GiB * 2 + 4 * GiB + 8 * GiB + GiB, "source plan does not add the matching budgets");
        check(planned_host_bytes(32 * GiB, true) == planned_host_bytes(32 * GiB, false) + 32 * GiB, "rectified plan omits the warp cache");

        MemoryReport r;
        r.host_process = 4 * GiB; r.host_available = 12 * GiB; r.host_total = 32 * GiB; r.host_planned = 8 * GiB;
        check(memory_risk(r) == MemoryRisk::Low, "half the supply is not low risk");
        r.host_planned = 14 * GiB;
        check(memory_risk(r) == MemoryRisk::Medium, "88% of supply is not medium risk");
        r.host_planned = 16 * GiB;
        check(memory_risk(r) == MemoryRisk::High, "the whole supply is not high risk");
        r.host_planned = 2 * GiB;
        check(host_demand_fraction(r) == 0.25, "usage above the plan was not taken as demand");

        r.device_known = true; r.device_capacity = 8 * GiB; r.device_others = 2 * GiB;
        r.device_ours = 3 * GiB; r.device_planned = 5 * GiB;
        check(memory_risk(r) == MemoryRisk::Medium, "the device ceiling did not raise the risk");
        r.device_planned = 6 * GiB;
        check(memory_risk(r) == MemoryRisk::High, "a full device is not high risk");
        r.device_known = false;
        check(device_demand_fraction(r) == 0 && memory_risk(r) == MemoryRisk::Low, "an unloaded device counted toward risk");
        check(memory_risk(MemoryReport{}) == MemoryRisk::Unknown, "an empty report has a risk");

        const auto dir = std::filesystem::temp_directory_path() / "spirula-dense-memory-report-test";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        MemoryReport read;
        check(!read_memory_report(dir, read), "a missing report was read");
        r.sequence = 7; r.phase = "match"; r.device_known = true;
        r.host_total = (1ull << 45) + 3;
        write_memory_report(dir, r);
        check(read_memory_report(dir, read), "report round trip failed");
        check(read.sequence == 7 && read.phase == "match" && read.host_total == r.host_total && read.host_process == r.host_process &&
              read.device_known && read.device_ours == r.device_ours && read.device_others == r.device_others &&
              read.device_capacity == r.device_capacity && read.device_planned == r.device_planned, "report fields changed in transit");
        { std::FILE* f = std::fopen(memory_report_path(dir).string().c_str(), "wb"); std::fputs("{\"sequence\": ", f); std::fclose(f); }
        check(!read_memory_report(dir, read) && read.sequence == 7, "a torn report replaced the last good one");
        std::filesystem::remove_all(dir);
    } catch (const std::exception& e) { std::printf("FAIL %s\n", e.what()); return 1; }
    std::printf("PASS dense memory plan, risk thresholds, device gating and report round trip\n");
    return 0;
}
