#pragma once

// The machine's side of a performance log: CPU per core, RAM, disk, this
// process and its children, Windows' per-adapter GPU engines and NVIDIA's own
// counters, sampled on a thread of its own into the files
// tools/perf/record_run.ps1 writes, so tools/perf/perf_report.py reads either.

#include <filesystem>
#include <memory>

namespace spirula {

class SystemRecorder {
public:
    SystemRecorder();
    ~SystemRecorder();
    SystemRecorder(const SystemRecorder&) = delete;
    SystemRecorder& operator=(const SystemRecorder&) = delete;

    // False when the folder cannot be written; samples until stop().
    bool start(const std::filesystem::path& dir, double interval_s = 1.0);
    void stop();
    bool running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace spirula
