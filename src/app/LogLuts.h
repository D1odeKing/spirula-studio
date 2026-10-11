#pragma once

// The log -> Rec.709 LUTs Spirula knows without a file: each maker's published
// log curve and gamut, then a look -- fitted by tools/fit_log_luts.py to how the
// maker's own LUT renders natural photographs, or GoPro's exact formulas --
// baked to a lattice the first time a run asks for one. docs/notes/log-luts.md.

#include "app/CubeLut.h"

#include <string>
#include <vector>

namespace app {

struct BuiltinLut {
    const char* id;    // what builtin_lut_spec() and --lut name
    const char* name;  // the maker's own name for the profile, untranslated
};

const std::vector<BuiltinLut>& builtin_luts();
const BuiltinLut* find_builtin_lut(const std::string& id);
// Its lattice; false with `error` set for an unknown id.
bool bake_builtin_lut(const std::string& id, CubeLut& out, std::string& error);

// The built-in a clip's own metadata says it was recorded for, as a spec; ""
// when it names none -- which includes every clip that was not shot in log.
std::string detect_log_lut(const std::string& video);

}  // namespace app
