// LogLuts.cpp -- see LogLuts.h.

#include "app/LogLuts.h"

#ifdef SS_TOOL_SFM
#include "sfm/core/Telemetry.h"
#endif

#include <algorithm>
#include <cmath>

namespace app {

namespace {

enum class LogCurve { SLog3, VLog, FLog, FLog2, DLog, ILog, CanonLog3, NLog, GpLog, GpLog2 };
// Fitted: m2 * curve_w(m1 * lin) + black, each of the kLogMixes mixes through
// its own monotone curve. Bt709: GoPro's technical LUT, m2 * OETF(lin). GpLog2:
// GoPro Labs' GP-Log2 generator.
enum class LogLook { Fitted, Bt709, GpLog2 };

constexpr int kLogLutSize = 33, kLogMixes = 6;
// Curve knots one stop apart, the first at 2^kLogKnot0 times 18% grey.
constexpr int kLogKnot0 = -8, kLogKnots = 15;

struct LogLutParams {
    BuiltinLut lut;
    LogCurve curve;
    LogLook look;
    double to709[9];  // the camera's gamut to linear Rec.709, row-major
    double m1[kLogMixes * 3], m2[3 * kLogMixes];
    double knots[kLogMixes * kLogKnots];
    double black[3];  // the look's lifted black, display code
};

}  // namespace
}  // namespace app

#include "app/LogLutTable.h"

namespace app {

namespace {

// The makers' published curves, code value / 1023 to scene linear with 18%
// grey at 0.18 -- GoPro's with 1 at clip. tools/fit_log_luts.py decode() cites
// each source and must agree with this one.
double decode(LogCurve c, double x) {
    switch (c) {
    case LogCurve::SLog3: {
        const double v = x * 1023;
        return v >= 171.2102946929 ? std::pow(10.0, (v - 420) / 261.5) * 0.19 - 0.01
                                   : (v - 95) * 0.01125 / (171.2102946929 - 95);
    }
    case LogCurve::VLog:
        return x < 0.181 ? (x - 0.125) / 5.6 : std::pow(10.0, (x - 0.598206) / 0.241514) - 0.00873;
    case LogCurve::FLog:
        return x < 0.100537775223865 ? (x - 0.092864) / 8.735631
                                     : (std::pow(10.0, (x - 0.790453) / 0.344676) - 0.009468) / 0.555556;
    case LogCurve::FLog2:
        return x < 0.100686685370811 ? (x - 0.092864) / 8.799461
                                     : (std::pow(10.0, (x - 0.384316) / 0.245281) - 0.064829) / 5.555556;
    case LogCurve::DLog:
        return x <= 0.14 ? (x - 0.0929) / 6.025
                         : (std::pow(10.0, 3.89616 * x - 2.27752) - 0.0108) / 0.9892;
    case LogCurve::ILog:
        return x < 0.154402 ? (x - 0.09055934) / 5.77837328
                            : std::pow(10.0, (x - 0.623992) / 0.280055) - 0.01;
    case LogCurve::CanonLog3: {
        const double t = (x * 1023 - 64) / 876;  // Canon Log 3 is defined on legal range
        const double v = t < 0.04076162 ? -(std::pow(10.0, (0.07623209 - t) / 0.42889912) - 1) / 14.98325
                         : t <= 0.105357102 ? (t - 0.073059361) / 2.3069815
                                            : (std::pow(10.0, (t - 0.069886632) / 0.42889912) - 1) / 14.98325;
        return 0.9 * v;
    }
    case LogCurve::NLog: {
        const double t = x / (650.0 / 1023);
        return x < 452.0 / 1023 ? t * t * t - 0.0075 : std::exp((x - 619.0 / 1023) / (150.0 / 1023));
    }
    case LogCurve::GpLog: return (std::pow(400.0, x) - 1) / 399;
    case LogCurve::GpLog2: return (std::pow(600.0, x) - 1) / 599;
    }
    return 0;
}

double oetf709(double v) {
    v = std::max(v, 0.0);
    return v < 0.018 ? 4.5 * v : 1.099 * std::pow(std::max(v, 0.018), 0.45) - 0.099;
}

// Fritsch-Butland slopes on unit-spaced, rising knots -- SciPy's PCHIP.
void pchip_slopes(const double* y, double* m) {
    double d[kLogKnots - 1];
    for (int k = 0; k + 1 < kLogKnots; k++) d[k] = y[k + 1] - y[k];
    for (int k = 1; k + 1 < kLogKnots; k++)
        m[k] = d[k - 1] * d[k] > 0 ? 2 * d[k - 1] * d[k] / (d[k - 1] + d[k] + 1e-300) : 0.0;
    auto edge = [](double d0, double d1) {
        const double e = (3 * d0 - d1) / 2;
        if (e * d0 <= 0) return 0.0;
        return d0 * d1 <= 0 && std::fabs(e) > 3 * std::fabs(d0) ? 3 * d0 : e;
    };
    m[0] = edge(d[0], d[1]);
    m[kLogKnots - 1] = edge(d[kLogKnots - 2], d[kLogKnots - 3]);
}

// Hermite between the knots on a log2 axis; linear in x to 0 below the first,
// flat past the last.
double curve(const double* y, const double* m, double x) {
    const double u = std::log2(std::max(x, 1e-30) / 0.18) - kLogKnot0;
    if (u <= 0) return y[0] * std::max(x, 0.0) / (0.18 * std::exp2(kLogKnot0));
    if (u >= kLogKnots - 1) return y[kLogKnots - 1];
    const int k = std::min((int)u, kLogKnots - 2);
    const double t = u - k, t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * y[k] + (t3 - 2 * t2 + t) * m[k] + (3 * t2 - 2 * t3) * y[k + 1] +
           (t3 - t2) * m[k + 1];
}

// GoPro Labs' GP-Log2 LUT generator (gopro.github.io/labs/gplog2, Apache-2.0;
// LICENSES/Apache-2.0-GoPro-Labs.txt) at its defaults: +1.8 EV, then its
// highlight rolloff, then the BT.709 OETF.
double gplog2_rolloff(double x, double max_in) {
    const double start = 0.25, end_slope = 0.01;
    if (x <= start) return x;
    if (x >= max_in) return 1.0;
    const double dx = max_in - start, avg = (1.0 - start) / dx;
    if (avg <= end_slope || avg >= 1.0) return x;
    const double p = (1.0 - end_slope) / (avg - end_slope) - 1.0, t = (x - start) / dx;
    return start + dx * (end_slope * t + (1.0 - end_slope) * (1.0 - std::pow(1.0 - t, p + 1.0)) / (p + 1.0));
}

double gplog2_toe(double v) {
    const double above = 1.0, maximum = 1.4, inv = 1 / (maximum - above);
    const double gain = (1.2 - above) / (maximum - above);
    const double n = 1 - v;
    if (n <= above) return v;
    const double a = std::min(1.0, n * inv);
    return 1 - ((n - above) * (a * gain + (1 - a)) + above);
}

void map(const LogLutParams& p, const double* slopes, const double code[3], float out[3]) {
    double cam[3], lin[3], v[3] = {p.black[0], p.black[1], p.black[2]};
    for (int c = 0; c < 3; c++) cam[c] = decode(p.curve, code[c]);
    for (int c = 0; c < 3; c++)
        lin[c] = p.to709[3 * c] * cam[0] + p.to709[3 * c + 1] * cam[1] + p.to709[3 * c + 2] * cam[2];
    switch (p.look) {
    case LogLook::Fitted:
        for (int w = 0; w < kLogMixes; w++) {
            const double x = p.m1[3 * w] * lin[0] + p.m1[3 * w + 1] * lin[1] + p.m1[3 * w + 2] * lin[2];
            const double y = curve(p.knots + w * kLogKnots, slopes + w * kLogKnots, x);
            for (int c = 0; c < 3; c++) v[c] += p.m2[c * kLogMixes + w] * y;
        }
        break;
    case LogLook::Bt709:
        for (int c = 0; c < 3; c++)
            for (int j = 0; j < 3; j++) v[c] += p.m2[c * kLogMixes + j] * oetf709(lin[j]);
        break;
    case LogLook::GpLog2: {
        const double gain = std::pow(2.0, 1.8);
        for (int c = 0; c < 3; c++) v[c] = oetf709(gplog2_toe(gplog2_rolloff(lin[c] * gain, gain)));
        break;
    }
    }
    for (int c = 0; c < 3; c++) out[c] = (float)std::clamp(v[c], 0.0, 1.0);
}

const LogLutParams* find_params(const std::string& id) {
    for (const LogLutParams& p : kLogLuts)
        if (id == p.lut.id) return &p;
    return nullptr;
}

}  // namespace

const std::vector<BuiltinLut>& builtin_luts() {
    static const std::vector<BuiltinLut> luts = [] {
        std::vector<BuiltinLut> v;
        for (const LogLutParams& p : kLogLuts) v.push_back(p.lut);
        return v;
    }();
    return luts;
}

const BuiltinLut* find_builtin_lut(const std::string& id) {
    for (const BuiltinLut& b : builtin_luts())
        if (id == b.id) return &b;
    return nullptr;
}

std::string detect_log_lut(const std::string& video) {
#ifdef SS_TOOL_SFM
    const sfm::VideoLogProfile p = sfm::video_log_profile(video);
    // DJI ships one D-Log M LUT for most of its cameras and a second for the
    // 2025 ones; the Avata 360 (wa530) is among those.
    const bool dji_2025 =
        p.camera == "dvtm_AVATA360.proto" || p.camera == "dvtm_eagle4_wa530.proto";
    const char* id = p.log == "dji-d-log-m" ? (dji_2025 ? "dji-dlog-m-2025" : "dji-dlog-m")
                     : p.log == "dji-d-log"   ? "dji-dlog"
                     : p.log == "gp-log"      ? "gopro-gplog"
                     : p.log == "s-log3-cine" ? "sony-slog3"
                     : p.log == "v-log"       ? "panasonic-vlog"
                                              : nullptr;
    if (id && find_builtin_lut(id)) return builtin_lut_spec(id);
#else
    (void)video;
#endif
    return {};
}

bool bake_builtin_lut(const std::string& id, CubeLut& out, std::string& error) {
    const LogLutParams* p = find_params(id);
    if (!p) {
        error = "no built-in LUT named " + id;
        return false;
    }
    double slopes[kLogMixes * kLogKnots];
    for (int w = 0; w < kLogMixes; w++) pchip_slopes(p->knots + w * kLogKnots, slopes + w * kLogKnots);
    const int n = kLogLutSize;
    out = CubeLut();
    out.size = n;
    out.title = p->lut.name;
    out.rgb.resize((size_t)n * n * n * 3);
    float* o = out.rgb.data();
    for (int b = 0; b < n; b++)
        for (int g = 0; g < n; g++)
            for (int r = 0; r < n; r++, o += 3) {
                const double code[3] = {(double)r / (n - 1), (double)g / (n - 1), (double)b / (n - 1)};
                map(*p, slopes, code, o);
            }
    return true;
}

}  // namespace app
