#pragma once
// For tests only: not included by any production file.

namespace nn::vk::testing {

// Pins Stream::workCap() (and the flush threshold) to `cap`, so a test can show
// an op's result does not depend on how the budget slices it. A negative value
// returns to the measured budget.
void override_work_cap(double cap);

}  // namespace nn::vk::testing
