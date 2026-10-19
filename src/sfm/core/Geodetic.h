// WGS-84 fixes and the local east-north-up metre frame a GPS reference is
// stated in. A model fitted to one is in that frame, so its origin is what
// georeferences the model (gauge.txt, sfm/Pipeline.h ModelGauge).
#pragma once

#include <cmath>
#include <string>
#include <vector>

#include "sfm/geometry/LinAlg.h"

namespace sfm {

struct Geodetic {
    double lat_deg = 0, lon_deg = 0, alt_m = 0;
};

// Where a local east-north-up frame has its (0,0,0). `alt` names what the
// height was read from ("exif", "dji", ...); empty when a fix had none.
struct GeoOrigin {
    bool ok = false;
    Geodetic at;
    std::string alt;
};

// WGS-84 geodetic to earth-centred earth-fixed, in double: ECEF magnitudes are
// ~6.4e6 m, where a float's half-ulp is 0.25 m.
inline Vec3 ecefFromGeodetic(double lat_deg, double lon_deg, double h) {
    constexpr double a = 6378137.0, f = 1.0 / 298.257223563;
    constexpr double e2 = f * (2.0 - f);
    const double p = lat_deg * M_PI / 180.0, l = lon_deg * M_PI / 180.0;
    const double sp = std::sin(p), cp = std::cos(p);
    const double N = a / std::sqrt(1.0 - e2 * sp * sp);
    return {(N + h) * cp * std::cos(l), (N + h) * cp * std::sin(l), (N * (1.0 - e2) + h) * sp};
}

// A local east-north-up metre frame about `origin`. Right-handed, north
// positive. Not valid across the antimeridian or over ~100 km.
inline std::vector<Vec3> enuFromGeodetic(const std::vector<Geodetic>& g,
                                         const Geodetic& origin) {
    std::vector<Vec3> out;
    if (g.empty()) return out;
    const double lat0 = origin.lat_deg, lon0 = origin.lon_deg, h0 = origin.alt_m;
    const Vec3 o = ecefFromGeodetic(lat0, lon0, h0);
    const double p = lat0 * M_PI / 180.0, l = lon0 * M_PI / 180.0;
    const double sp = std::sin(p), cp = std::cos(p), sl = std::sin(l), cl = std::cos(l);
    out.reserve(g.size());
    for (const Geodetic& q : g) {
        const Vec3 d = ecefFromGeodetic(q.lat_deg, q.lon_deg, q.alt_m) - o;
        out.push_back({-sl * d.x + cl * d.y,
                       -sp * cl * d.x - sp * sl * d.y + cp * d.z,
                       cp * cl * d.x + cp * sl * d.y + sp * d.z});
    }
    return out;
}

inline Geodetic meanGeodetic(const std::vector<Geodetic>& g) {
    Geodetic o;
    if (g.empty()) return o;
    for (const Geodetic& q : g) {
        o.lat_deg += q.lat_deg;
        o.lon_deg += q.lon_deg;
        o.alt_m += q.alt_m;
    }
    const double inv = 1.0 / (double)g.size();
    o.lat_deg *= inv;
    o.lon_deg *= inv;
    o.alt_m *= inv;
    return o;
}

// The same, about the mean of the fixes. The rotation depends on where the
// origin is, so two origins do not differ by a translation alone.
inline std::vector<Vec3> enuFromGeodetic(const std::vector<Geodetic>& g) {
    return enuFromGeodetic(g, meanGeodetic(g));
}

}  // namespace sfm
