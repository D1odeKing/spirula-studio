// The public Extractor: one image in, DaD keypoints with DeDoDe descriptions
// out. Two networks, two resolutions, and the normalized keypoints that pass
// between them.

#include "loma/Loma.h"

#include "loma/Common.h"
#include "loma/model/Dump.h"
#include "loma/model/Model.h"
#include "nn/Ops.h"
#include "nn/Tensor.h"
#include "nn/io/Resize.h"
#include "nn/vk/EmbeddedSpirv.h"
#include "nn/vk/Memory.h"
#include "nn/vk/Stream.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// This library's SPIR-V blobs, registered by an explicit call from load():
// ss_loma is a static archive, and an object nothing references is not linked.
NN_DECLARE_EMBEDDED_MODULES(loma)

namespace loma {
namespace {

using nn::DType;
using nn::Tensor;

// rgb bytes -> [ho, wo, 3] floats, scaled by 1/255 and then by (x - mean) / sd.
std::vector<float> prepare(const uint8_t* rgb, int wi, int hi, int64_t wo, int64_t ho,
                           const float* mean, const float* sd) {
    std::vector<float> a((size_t)hi * wi * 3);
    nn::parallel_for((int64_t)a.size(), [&](int64_t i0, int64_t i1) {
        for (int64_t i = i0; i < i1; ++i) a[(size_t)i] = rgb[i] * (1.0f / 255.0f);
    });

    a = nn::resize_rgb_bicubic(a.data(), wi, hi, (int)wo, (int)ho);

    nn::parallel_for((int64_t)a.size(), [&](int64_t i0, int64_t i1) {
        for (int64_t i = i0; i < i1; ++i)
            a[(size_t)i] = (a[(size_t)i] - mean[i % 3]) / sd[i % 3];
    });
    return a;
}

}  // namespace

struct Extractor::Impl {
    Detector   det;
    Descriptor desc;
    vk::Arena  arena{"loma"};
    uint64_t   planned = 0;
};

Extractor::Extractor() : impl_(new Impl) {}
Extractor::~Extractor() { delete impl_; }
bool Extractor::loaded() const { return impl_->det.loaded() && impl_->desc.loaded(); }
int  Extractor::descriptorDim() const { return impl_->desc.dim(); }
uint64_t Extractor::plannedBytes() const { return impl_->planned; }
uint64_t Extractor::peakBytes() const { return impl_->arena.highWater(); }

void Extractor::load(const std::string& detector, const std::string& descriptor) {
    NN_ENSURE_EMBEDDED_MODULES(loma);
    impl_->det.load(detector, &impl_->arena);
    impl_->desc.load(descriptor, &impl_->arena);
}

Features Extractor::extract(const uint8_t* rgb, int width, int height,
                            const ExtractOptions& opts) {
    NN_CHECK(loaded(), "Extractor::extract before load()");
    NN_CHECK(rgb != nullptr && width > 0 && height > 0, "Extractor::extract: empty image");
    Impl& im = *impl_;

    Features out;
    out.width = width;
    out.height = height;
    out.desc_dim = im.desc.dim();

    // No padding and no crop: the encoder's three halvings floor, and
    // floor(floor(n/2)/2) is floor(n/4) for every n, so the cascade's resize
    // targets line up with the taps at any size. COLMAP hands DaD the bitmap.
    const int64_t H = height, W = width;
    NN_CHECK(H >= 8 && W >= 8, "image is %dx%d; LoMa needs at least 8x8", width, height);

    const int64_t S = im.desc.inputSize();
    const int64_t plan = std::max(im.det.planBytes(H, W), im.desc.planBytes());
    // The largest plan so far, because the arena's high water is cumulative
    // too: a small image after a large one must not read as an overflow.
    im.planned = std::max(im.planned, (uint64_t)plan);
    try {
        im.arena.reserve((uint64_t)plan);
    } catch (const nn::Error& e) {
        // The allocator names the bytes and the device; what it cannot know is
        // that almost every term here is linear in the pixel count, which
        // makes the working resolution the knob. Quote the next step down.
        const int64_t edge = std::max(H, W), smaller = edge * 3 / 4 / 8 * 8;
        const double sc = (double)smaller / (double)edge;
        nn::fail("%s\nLoMa's working set is %.2f GB at %lldx%lld and grows with "
                 "the pixel count, so --max-image-size is the knob: %lld px "
                 "would need about %.2f GB.",
                 e.what(), (double)plan / 1e9, (long long)W, (long long)H,
                 (long long)smaller,
                 (double)std::max(im.det.planBytes((int64_t)(H * sc), (int64_t)(W * sc)),
                                  im.desc.planBytes()) / 1e9);
    }

    DetectorOut kp;
    {
        vk::ArenaScope root(im.arena);
        Tensor img = nn::arena_tensor(im.arena, DType::F32, H, W, 3);
        {
            const std::vector<float> host =
                prepare(rgb, width, height, W, H, im.det.mean(), im.det.std());
            nn::tensor_from_host(img, host.data(), (int64_t)host.size());
        }
        dump_tensor("det_input", img, {H, W, 3});
        kp = im.det.run(img, H, W, opts.max_num_features, opts.min_score,
                        opts.max_candidates);
    }
    const int n = (int)kp.score.size();
    if (n == 0) return out;

    out.descriptors.resize((size_t)n * out.desc_dim);
    {
        vk::ArenaScope root(im.arena);
        Tensor img = nn::arena_tensor(im.arena, DType::F32, S, S, 3);
        {
            const float zero[3] = {0, 0, 0}, one[3] = {1, 1, 1};
            const std::vector<float> host = prepare(rgb, width, height, S, S, zero, one);
            nn::tensor_from_host(img, host.data(), (int64_t)host.size());
        }
        im.desc.run(img, kp.xy.data(), n, out.descriptors.data());
    }

    // Normalized -> pixels, corner origin: exactly COLMAP's conversion in
    // LomaFeatureExtractor::Extract, so the two sides agree on where a
    // keypoint is.
    out.keypoints.resize(n);
    for (int i = 0; i < n; ++i) {
        out.keypoints[i].x = 0.5f * (kp.xy[(size_t)i * 2] + 1.0f) * (float)W;
        out.keypoints[i].y = 0.5f * (kp.xy[(size_t)i * 2 + 1] + 1.0f) * (float)H;
        out.keypoints[i].score = kp.score[(size_t)i];
    }
    return out;
}

std::string descriptor_for_matcher(const std::string& matcher) {
    if (matcher == "loma-b128") return "loma-dedode-b";
    if (matcher == "loma-b" || matcher == "loma-r" || matcher == "loma-l" ||
        matcher == "loma-g")
        return "loma-dedode-g";
    return "";
}

}  // namespace loma
