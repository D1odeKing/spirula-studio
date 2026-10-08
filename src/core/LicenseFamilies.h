#pragma once
// The names of the built-in licence families: the unit a user accepts, and what
// a downloadable file names in FetchFile::license_family. Constants only, so the
// models that name one need not link the table (core/LicenseConsent.h).

namespace spirula::license::family {

inline constexpr const char* kSam3 = "sam3";         // Meta's SAM License
inline constexpr const char* kSam2 = "sam2";         // SAM 2.1, Apache-2.0
inline constexpr const char* kGdino = "gdino";       // Grounding DINO, Apache-2.0
inline constexpr const char* kBirefnet = "birefnet"; // BiRefNet, MIT

}  // namespace spirula::license::family
