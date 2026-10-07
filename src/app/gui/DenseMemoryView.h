#pragma once

// The dense step's memory on the dataset screen: an out-of-memory risk tag and
// RAM / VRAM bars in a strip above the log, read from the child's report
// (dense/MemoryReport.h), with the history and planned ceiling on hover.

#include "dense/MemoryReport.h"

#include <string>
#include <vector>

namespace gui {

class DenseMemoryView {
public:
    // Call every frame while the dense step runs; reads the report at most twice a second.
    void poll(const std::string& progress_dir);
    // Forget the previous run's history; the next poll starts a new one.
    void reset();
    // One right-aligned row inside the strip that starts at `x0` and is `avail` wide.
    void draw(float x0, float avail);
    bool visible() const { return have_; }

private:
    struct Sample { double seconds; double host, device, others; };
    spirula::dense::MemoryReport latest_;
    std::vector<Sample> history_;
    double polled_at_ = -1.0, started_at_ = -1.0;
    uint64_t sequence_ = 0;
    bool have_ = false;
};

}  // namespace gui
