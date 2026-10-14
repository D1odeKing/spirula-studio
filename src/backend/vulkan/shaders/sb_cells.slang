#pragma once

// How the per-cell optimizers treat the lobe cells of a features_sh row
// (core/ColorBasis.h, SbOptimCells): their own rate, no colour trust region
// and no regularizer.

#include "shaders/spherical_beta.slang"

static const uint kSbNoLobes = 0xffffffffu;
