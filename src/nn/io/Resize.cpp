#include "nn/core/Parallel.h"
#include "nn/io/Image.h"
#include "nn/io/Resize.h"
#include "nn/core/Error.h"

#include <algorithm>
#include <cmath>

namespace nn {
namespace {

// One output coordinate's taps: PIL's precompute_coeffs for the bilinear
// filter, normalized so the weights sum to one.
struct Taps {
    std::vector<int>   first, count;
    std::vector<float> w;   // count.max() per output
    int                stride = 0;
};

Taps taps(int in, int out) {
    Taps t;
    const double scale = (double)in / out;
    const double support = std::max(scale, 1.0);   // bilinear's radius is 1
    t.stride = (int)std::ceil(support) * 2 + 1;
    t.first.resize((size_t)out);
    t.count.resize((size_t)out);
    t.w.assign((size_t)out * t.stride, 0.0f);
    const double ss = 1.0 / std::max(scale, 1.0);
    for (int o = 0; o < out; ++o) {
        const double center = (o + 0.5) * scale;
        const int x0 = std::max(0, (int)(center - support + 0.5));
        const int x1 = std::min(in, (int)(center + support + 0.5));
        double sum = 0.0;
        for (int x = x0; x < x1; ++x) {
            const double d = std::fabs((x - center + 0.5) * ss);
            const double v = d < 1.0 ? 1.0 - d : 0.0;
            t.w[(size_t)o * t.stride + (x - x0)] = (float)v;
            sum += v;
        }
        if (sum > 0)
            for (int x = x0; x < x1; ++x) t.w[(size_t)o * t.stride + (x - x0)] /= (float)sum;
        t.first[(size_t)o] = x0;
        t.count[(size_t)o] = x1 - x0;
    }
    return t;
}

// Cubic support widens when shrinking, as in PIL and torch antialiasing.
float pil_cubic(float t) {
    const float a = -0.5f;
    t = std::fabs(t);
    if (t <= 1.0f) return ((a + 2.0f) * t - (a + 3.0f)) * t * t + 1.0f;
    if (t < 2.0f) return ((a * t - 5.0f * a) * t + 8.0f * a) * t - 4.0f * a;
    return 0.0f;
}

// One separable pass over interleaved RGB. `other` walks the axis that is not
// being resampled; `step` walks the one that is.
void resample_axis(const float* src, float* dst, int64_t n_other, int64_t n_in,
                   int64_t n_out, int64_t src_other, int64_t dst_other, int64_t src_step,
                   int64_t dst_step, BicubicWeights weights) {
    const bool fp32 = weights == BicubicWeights::Float32;
    const double scale = fp32 ? (double)((float)n_in / (float)n_out) : (double)n_in / (double)n_out;
    const double filter_scale = std::max(scale, 1.0);
    const double support = 2.0 * filter_scale;
    const double inv = fp32 ? (double)(float)(1.0 / filter_scale) : 1.0 / filter_scale;
    parallel_for(n_out, [&](int64_t o0, int64_t o1) {
        std::vector<float> wbuf;
        for (int64_t o = o0; o < o1; ++o) {
            const double position = ((double)o + 0.5) * scale;
            const double centre = fp32 ? (double)(float)position : position;
            const int64_t lo = std::max<int64_t>((int64_t)(centre - support + 0.5), 0);
            const int64_t hi = std::min<int64_t>((int64_t)(centre + support + 0.5), n_in);
            wbuf.assign((size_t)std::max<int64_t>(hi - lo, 1), 0.0f);
            float wsum = 0;
            for (int64_t i = lo; i < hi; ++i) {
                const double distance = fp32 ? (double)((float)i - (float)centre) + 0.5 : ((double)i + 0.5) - centre;
                const float w = pil_cubic((float)(distance * inv));
                wbuf[(size_t)(i - lo)] = w;
                wsum += w;
            }
            if (wsum == 0.0f) wsum = 1.0f;
            if (fp32) for (float& weight : wbuf) weight /= wsum;
            for (int64_t k = 0; k < n_other; ++k) {
                const float* sp = src + k * src_other;
                float* d = dst + k * dst_other + o * dst_step;
                for (int c = 0; c < 3; ++c) {
                    float acc = 0;
                    for (int64_t i = lo; i < hi; ++i)
                        acc += wbuf[(size_t)(i - lo)] * sp[i * src_step + c];
                    d[c] = fp32 ? acc : acc / wsum;
                }
            }
        }
    });
}

}  // namespace

Image resize_image(const Image& src, int width, int height) {
    if (src.empty() || (src.width == width && src.height == height)) return src;
    const int C = src.channels;
    const Taps tx = taps(src.width, width), ty = taps(src.height, height);

    // Horizontal pass into float rows, then vertical.
    std::vector<float> mid((size_t)src.height * width * C);
    parallel_for(src.height, [&](int64_t lo, int64_t hi) {
        for (int64_t y = lo; y < hi; ++y) {
            const uint8_t* row = src.data.data() + (size_t)y * src.width * C;
            float* dst = mid.data() + (size_t)y * width * C;
            for (int x = 0; x < width; ++x) {
                const float* w = &tx.w[(size_t)x * tx.stride];
                for (int c = 0; c < C; ++c) {
                    float acc = 0.0f;
                    for (int k = 0; k < tx.count[(size_t)x]; ++k)
                        acc += w[k] * row[(size_t)(tx.first[(size_t)x] + k) * C + c];
                    dst[(size_t)x * C + c] = acc;
                }
            }
        }
    }, 8);

    Image out;
    out.width = width;
    out.height = height;
    out.channels = C;
    out.data.resize((size_t)width * height * C);
    parallel_for(height, [&](int64_t lo, int64_t hi) {
        for (int64_t y = lo; y < hi; ++y) {
            const float* w = &ty.w[(size_t)y * ty.stride];
            uint8_t* dst = out.data.data() + (size_t)y * width * C;
            for (int i = 0; i < width * C; ++i) {
                float acc = 0.0f;
                for (int k = 0; k < ty.count[(size_t)y]; ++k)
                    acc += w[k] * mid[(size_t)(ty.first[(size_t)y] + k) * width * C + i];
                dst[i] = (uint8_t)std::min(255.0f, std::max(0.0f, std::round(acc)));
            }
        }
    }, 8);
    return out;
}

std::vector<float> resize_rgb_bicubic(const float* rgb, int width, int height,
                                       int output_width, int output_height, BicubicWeights weights) {
    NN_CHECK(rgb && width > 0 && height > 0 && output_width > 0 && output_height > 0,
             "bicubic resize expects nonempty input and positive dimensions");
    std::vector<float> a(rgb, rgb + (size_t)height * width * 3);
    if (output_width != width) {
        std::vector<float> b((size_t)height * output_width * 3);
        resample_axis(a.data(), b.data(), height, width, output_width,
                      (int64_t)width * 3, (int64_t)output_width * 3, 3, 3, weights);
        a.swap(b);
    }
    if (output_height != height) {
        std::vector<float> b((size_t)output_height * output_width * 3);
        resample_axis(a.data(), b.data(), output_width, height, output_height,
                      3, 3, (int64_t)output_width * 3, (int64_t)output_width * 3, weights);
        a.swap(b);
    }
    return a;
}

}  // namespace nn
