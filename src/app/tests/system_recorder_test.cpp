// system_recorder_test -- SystemRecorder against this process: it burns a core
// for a while and checks the rows have the report's columns, see the load, and
// stop cleanly. NVIDIA rows are checked only where a driver is present.

#include "app/SystemRecorder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_failures++;
}

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    std::stringstream in(line);
    for (std::string cell; std::getline(in, cell, ',');) out.push_back(cell);
    if (!line.empty() && line.back() == ',') out.emplace_back();
    return out;
}

// Header and rows of a CSV, each row a column -> cell map.
std::pair<std::vector<std::string>, std::vector<std::map<std::string, std::string>>> read_csv(const fs::path& p) {
    std::ifstream in(p);
    std::string line;
    std::vector<std::string> header;
    std::vector<std::map<std::string, std::string>> rows;
    if (std::getline(in, line)) header = split(line);
    while (std::getline(in, line)) {
        const auto cells = split(line);
        if (cells.size() != header.size()) {
            check(false, p.filename().string() + ": a row with " + std::to_string(cells.size()) +
                             " cells under " + std::to_string(header.size()) + " columns");
            continue;
        }
        std::map<std::string, std::string> row;
        for (size_t i = 0; i < cells.size(); ++i) row[header[i]] = cells[i];
        rows.push_back(row);
    }
    return {header, rows};
}

double value(const std::map<std::string, std::string>& row, const char* key) {
    const auto it = row.find(key);
    return it == row.end() || it->second.empty() ? -1.0 : std::atof(it->second.c_str());
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path dir = fs::temp_directory_path() / "spirula-system-recorder-test";
    fs::remove_all(dir);

    std::atomic<bool> burn{true};
    std::thread busy([&] {
        volatile double x = 1.0;
        while (burn.load()) x = x * 1.0000001 + 1e-9;
    });
    spirula::SystemRecorder rec;
    check(rec.start(dir, 0.25), "starts");
    check(rec.running(), "runs");
    std::this_thread::sleep_for(std::chrono::milliseconds(1800));
    rec.stop();
    burn = false;
    busy.join();
    check(!rec.running(), "stops");

    const auto [header, rows] = read_csv(dir / "system.csv");
    check(header.size() > 16 && header.front() == "unix_ms" && header.back() == "spirula_write_bps",
          "system.csv has the report's columns");
    check(rows.size() >= 4, "a row every quarter second (" + std::to_string(rows.size()) + ")");
    bool time_rises = true, procs = true, ram = true, rss = true;
    double cpu_max = -1.0, total_max = -1.0;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i && value(rows[i], "unix_ms") <= value(rows[i - 1], "unix_ms")) time_rises = false;
        procs = procs && value(rows[i], "spirula_procs") >= 1.0;
        ram = ram && value(rows[i], "ram_total_bytes") > value(rows[i], "ram_avail_bytes") &&
              value(rows[i], "ram_avail_bytes") > 0.0;
        rss = rss && value(rows[i], "spirula_ws_bytes") > 0.0;
        cpu_max = std::max(cpu_max, value(rows[i], "spirula_cpu_pct"));
        total_max = std::max(total_max, value(rows[i], "cpu_total"));
    }
    check(time_rises, "timestamps rise");
    check(procs, "counts this process");
    check(ram, "machine memory in range");
    check(rss, "process memory present");
#if defined(_WIN32) || defined(__linux__)
    check(cpu_max > 40.0, "sees the busy core in this process (" + std::to_string(cpu_max) + "%)");
    check(total_max > 0.0 && total_max <= 100.0, "machine CPU in range (" + std::to_string(total_max) + "%)");
#endif

    const auto [eng_header, eng_rows] = read_csv(dir / "gpu_engines.csv");
    check(eng_header.size() == 5 && eng_header[2] == "luid", "gpu_engines.csv header");

    {
        std::ifstream meta(dir / "meta.json");
        std::stringstream ms;
        ms << meta.rdbuf();
        check(ms.str().find("\"logical_processors\"") != std::string::npos, "meta.json written");
    }

    if (fs::exists(dir / "nvidia.csv")) {
        const auto [nv_header, nv_rows] = read_csv(dir / "nvidia.csv");
        check(nv_header.size() == 13 && !nv_rows.empty(), "nvidia.csv rows (" + std::to_string(nv_rows.size()) + ")");
        bool sane = !nv_rows.empty();
        for (const auto& r : nv_rows)
            sane = sane && value(r, "memory_total") > 0.0 && value(r, "utilization_gpu") >= 0.0 &&
                   r.at("pstate").rfind("P", 0) == 0 && r.at("clocks_event_reasons_active").rfind("0x", 0) == 0;
        check(sane, "nvidia.csv values in range");
    } else {
        std::printf("skip no NVIDIA driver\n");
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    std::printf("%s\n", g_failures ? "system_recorder_test: FAILED" : "system_recorder_test: OK");
    return g_failures ? 1 : 0;
}
