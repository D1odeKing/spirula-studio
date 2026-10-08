#pragma once
// The licence families a model brings beyond the four built into
// core/LicenseConsent.h. Every entry point that asks about licences registers
// them first: Main.cpp for --accept-license and the CLI gate, the GUI for its dialog.

#include "core/LicenseConsent.h"
#include "core/LicenseTexts.h"
#include "roma/model/Fetch.h"

#include <string>

namespace app {

inline void register_model_licenses() {
    // One family, two licences: the RoMa checkpoint carries DINOv3's weights.
    static const std::string roma_text = std::string(spirula::license::kDinov3License) +
                                         "\n---\n\n" + spirula::license::kRomaMit;
    static const spirula::license::Terms roma{
        spirula::roma::kLicenseFamily, "DINOv3 License (Meta) and RoMa v2 License (MIT)",
        spirula::roma::kDinoTerms, roma_text.c_str()};
    spirula::license::register_terms(&roma);
}

}  // namespace app
