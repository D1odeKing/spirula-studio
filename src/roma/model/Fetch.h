#pragma once

#include "nn/io/Fetch.h"

namespace spirula::roma {
// Registered by app/ModelLicenses.h: DINOv3's terms and RoMa's MIT, accepted as one.
inline constexpr const char* kLicenseFamily = "roma";
inline constexpr nn::FetchFile kOfficialCheckpoint{
    "romav2.0.1.pt",
    "https://github.com/Parskatt/RoMaV2/releases/download/v2.0.1/romav2.0.1.pt",
    "1557dec0d21b62366465f7ff4d5fdf228cc695d0582e196ad2b80e05230828b7",
    1095883548ull, nullptr, kLicenseFamily};
inline constexpr const char* kRomaTerms = "https://github.com/Parskatt/RoMaV2/blob/95c9968145c8906b7b59383258e9f73b02853d89/LICENSE";
inline constexpr const char* kDinoTerms = "https://github.com/facebookresearch/dinov3/blob/adc254450203739c8149213a7a69d8d905b4fcfa/LICENSE.md";
}
