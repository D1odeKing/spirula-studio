#include "dense/MemoryReport.h"

#include "core/AtomicFile.h"
#include "data/Json.h"
#include "data/JsonWrite.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace spirula::dense {

double host_demand_fraction(const MemoryReport& r) {
    const double supply = (double)r.host_process + (double)r.host_available;
    return supply > 0 ? (double)std::max(r.host_planned, r.host_process) / supply : 0;
}

double device_demand_fraction(const MemoryReport& r) {
    if (!r.device_known || !r.device_capacity) return 0;
    return ((double)std::max(r.device_planned, r.device_ours) + (double)r.device_others) / (double)r.device_capacity;
}

MemoryRisk memory_risk(double demand) {
    return demand <= 0 ? MemoryRisk::Unknown : demand < 0.8 ? MemoryRisk::Low : demand < 0.95 ? MemoryRisk::Medium : MemoryRisk::High;
}

MemoryRisk memory_risk(const MemoryReport& r) {
    return memory_risk(std::max(host_demand_fraction(r), device_demand_fraction(r)));
}

uint64_t planned_host_bytes(uint64_t budget, bool rectified) {
    // Decoded pixels, queued predictions, pending tiles (1/8), reference workers (1/4),
    // the paged job table (1/32), and for rectified runs the warp-plan cache.
    return budget * 2 + budget / 8 + budget / 4 + budget / 32 + (rectified ? budget : 0);
}

std::filesystem::path memory_report_path(const std::filesystem::path& progress_dir) {
    return progress_dir / "memory.json";
}

void write_memory_report(const std::filesystem::path& progress_dir, const MemoryReport& r) {
    JsonWriter json; json.object();
    json.field("sequence", (long long)r.sequence).field("phase", r.phase);
    json.field("host_process", (long long)r.host_process).field("host_available", (long long)r.host_available);
    json.field("host_total", (long long)r.host_total).field("host_planned", (long long)r.host_planned);
    json.field("device_known", r.device_known);
    json.field("device_ours", (long long)r.device_ours).field("device_others", (long long)r.device_others);
    json.field("device_capacity", (long long)r.device_capacity).field("device_planned", (long long)r.device_planned);
    json.end();
    const auto destination = memory_report_path(progress_dir);
    const auto part = destination.string() + ".part";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        out << json.str();
        if (!out.flush()) return;
    }
    try { replace_file(part, destination); } catch (const std::exception&) {}   // a reader holds it; the next write retries
}

bool read_memory_report(const std::filesystem::path& progress_dir, MemoryReport& r) {
    std::ifstream in(memory_report_path(progress_dir), std::ios::binary);
    if (!in) return false;
    std::stringstream text; text << in.rdbuf();
    try {
        const JsonValue json = json_parse(text.str());
        if (!json.is_object()) return false;
        auto bytes = [&](const char* key) { const auto* v = json.find(key); return v ? (uint64_t)std::max<int64_t>(0, v->as_int()) : 0; };
        MemoryReport out;
        out.sequence = bytes("sequence");
        if (const auto* phase = json.find("phase")) out.phase = phase->as_string();
        out.host_process = bytes("host_process"); out.host_available = bytes("host_available");
        out.host_total = bytes("host_total"); out.host_planned = bytes("host_planned");
        if (const auto* known = json.find("device_known")) out.device_known = known->as_bool();
        out.device_ours = bytes("device_ours"); out.device_others = bytes("device_others");
        out.device_capacity = bytes("device_capacity"); out.device_planned = bytes("device_planned");
        r = std::move(out);
        return true;
    } catch (const std::exception&) { return false; }
}

}  // namespace spirula::dense
