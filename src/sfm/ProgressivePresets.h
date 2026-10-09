#pragma once

// Progressive alignment's settings per --quality level, read by the CLI's
// presets and by the GUI's quality control alike. Measured on a 260-image
// capture (docs/notes/sfm-progressive-alignment.md): past five attempts the
// ladder placed nothing more, and a feature pass costs about one attempt.

namespace sfm {

struct ProgressivePreset {
    float error_start;   // px; the end is the run's --max-error
    int error_steps;     // attempts
    bool features;       // feature passes on the images left out
    int feature_steps;
    int patience;
};

// low, medium, high, extreme: the GUI's quality index.
inline constexpr ProgressivePreset kProgressivePresets[4] = {
    {12.0f, 3, false, 1, 1},
    {16.0f, 4, true, 1, 1},
    {20.0f, 5, true, 2, 2},
    {24.0f, 7, true, 3, 2},
};

}  // namespace sfm
