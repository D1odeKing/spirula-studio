#pragma once

// The dense child's memory, published beside its live snapshot so the GUI can
// draw a bar for a process it did not start the device in. Bytes throughout;
// "planned" is the ceiling the run's own budgets allow, which is the projection.

#include <cstdint>
#include <filesystem>
#include <string>

namespace spirula::dense {

struct MemoryReport {
    uint64_t sequence = 0;
    std::string phase;
    uint64_t host_process = 0, host_available = 0, host_total = 0, host_planned = 0;
    bool device_known = false;
    uint64_t device_ours = 0, device_others = 0, device_capacity = 0, device_planned = 0;
};

enum class MemoryRisk { Unknown, Low, Medium, High };

// Demand at the planned ceiling over what the machine can supply: below 0.8 is
// low, below 0.95 medium. Host supply is what this process holds plus what is free.
double host_demand_fraction(const MemoryReport& report);
double device_demand_fraction(const MemoryReport& report);
MemoryRisk memory_risk(double demand_fraction);
MemoryRisk memory_risk(const MemoryReport& report);

// The matching stage's host budgets added up; each is a share of `budget` in
// app/DenseProcessing.cpp and src/dense/Reconstruction.cpp, and must follow them.
uint64_t planned_host_bytes(uint64_t budget, bool rectified);

std::filesystem::path memory_report_path(const std::filesystem::path& progress_dir);
void write_memory_report(const std::filesystem::path& progress_dir, const MemoryReport& report);
bool read_memory_report(const std::filesystem::path& progress_dir, MemoryReport& report);

}  // namespace spirula::dense
