#pragma once
// Licence consent for a SAM checkpoint named on the command line (`--model`).
//
// The SAM checkpoints are a file the user points at, so nothing here downloads
// anything; but loading one is still agreeing to Meta's terms, and the GUI asks
// for that before it fetches the same file. This asks the same question of a
// terminal: the whole text, then `yes`. No terminal: it refuses, naming
// `--accept-license <family>=yes`. The family comes from the file's name
// (sam3*, sam2*, the names the catalogue downloads under); any other name has
// no SAM family and is left alone, so BiRefNet and Grounding DINO checkpoints
// are gated where they are fetched instead (nn::ensure_file).

#include "app/cli/LicenseCli.h"
#include "core/LicenseFamilies.h"

#include <cctype>
#include <cstdio>
#include <exception>
#include <string>

namespace app {

// "sam3" or "sam2" from a checkpoint path's file name, else null.
inline const char* sam_license_family(const std::string& model) {
    const size_t cut = model.find_last_of("/\\");
    std::string base = cut == std::string::npos ? model : model.substr(cut + 1);
    for (char& c : base) c = (char)std::tolower((unsigned char)c);
    if (base.rfind("sam3", 0) == 0) return spirula::license::family::kSam3;
    if (base.rfind("sam2", 0) == 0) return spirula::license::family::kSam2;
    return nullptr;
}

// True when the checkpoint may be loaded. Otherwise the reason is on stderr and
// the caller exits non-zero before it opens the file.
inline bool require_model_license(const std::string& model,
                                  const ConsentIO& io = stdio_consent()) {
    const char* family = sam_license_family(model);
    if (!family) return true;
    try {
        require_license(family, io);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return false;
    }
    return true;
}

}  // namespace app
