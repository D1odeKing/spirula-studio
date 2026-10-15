#pragma once

// Masking several camera folders at once. The neural-net layer records every
// dispatch into one process-wide stream, so two maskers in one process would
// interleave their GPU work; each folder runs instead in a child of this same
// executable (`spirula gui --mask-worker JOB`), with its own GPU context, and
// reports back on stdout. docs/notes/staging.md.

#include "app/gui/DatasetPrep.h"

#include <string>

namespace gui {

struct MaskTask {
    PrepJob job;              // its mask_* fields, device and colour settings
    PrepInput input;          // path, is_video, packed_lenses and its stencil
    std::string images, masks, feature_masks;
    bool train = true;        // write training masks (not only feature masks)
};

std::string encode_mask_task(const MaskTask& t);
bool decode_mask_task(const std::string& json, MaskTask& t, std::string& error);

// The worker's lines: "@@progress DONE TOTAL", "@@log 0|1 TEXT", "@@error TEXT",
// and last "@@folded 0|1" (whether the stencil went into the masks it wrote).
bool parse_worker_progress(const std::string& line, long long& done, long long& total);

// How many folders to mask at once: `requested` when above 0, else free GPU
// memory over ~3 GB a stream (SAM2.1 and its detector), at most 8; 1 when the
// free memory cannot be read. Never more than `folders`.
int mask_stream_count(int requested, size_t folders, const std::string& device);

// `spirula gui --mask-worker JOB.json`: masks one folder and exits 0, or 1
// with an "@@error" line.
int mask_worker_main(const std::string& job_file);

}  // namespace gui
