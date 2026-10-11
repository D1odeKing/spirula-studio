// The gauge from the attitude each image records (core/Attitude.h): up from
// its pitch and roll, north from its yaw. Every registered image carrying one
// votes, and a set the reconstruction contradicts is refused rather than
// averaged. Orient.h guesses up from how the cameras were held; this measures
// it, which a capture looking down, or one a gimbal turned upside down, needs.
#pragma once

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "sfm/core/Attitude.h"
#include "sfm/core/Exif.h"
#include "sfm/core/Model.h"
#include "sfm/core/Pose.h"
#include "sfm/map/ImuExtrinsic.h"
#include "sfm/map/Orient.h"

namespace sfm {

// Camera -> east-north-up, for each registered image that records an attitude.
struct AttitudeRef {
    std::vector<uint32_t> image_ids;
    std::vector<Mat3> world_from_cam;
    int registered = 0;
};

// `pixels_turned` is `--exif-orientation apply`, under which a model camera is
// the turned frame rather than the one the attitude describes. Images under a
// `skip` prefix are not read.
inline AttitudeRef attitudeRefFromImages(const Reconstruction& rec, const std::string& image_dir,
                                         bool pixels_turned,
                                         const std::vector<std::string>& skip = {}) {
    AttitudeRef ref;
    for (const auto& kv : rec.images) {
        if (!kv.second.registered) continue;
        ref.registered++;
        if (exifPrefixCovers(skip, kv.second.name)) continue;
        const std::string path = (std::filesystem::path(image_dir) / kv.second.name).string();
        const CameraAttitude a = readCameraAttitude(path);
        if (!a.valid) continue;
        if (pixels_turned && !exifTransform(exifOrientation(path)).identity()) continue;
        ref.image_ids.push_back(kv.first);
        ref.world_from_cam.push_back(attitudeWorldFromCamera(a));
    }
    return ref;
}

enum class AttitudeFail { None, Few, Disagree };

struct AttitudeFit {
    bool ok = false;        // T levels the model
    bool north = false;     // ... and turns north onto +Y
    AttitudeFail reason = AttitudeFail::Few;
    AttitudeFail north_reason = AttitudeFail::None;
    Sim3 T;
    UpConsensus up;         // in the model's frame
    UpConsensus heading;    // (cos, sin, 0) of the turn about the levelled +Z
};

// A vote more than 10 degrees off is an outlier (consensusUp); a set with more
// outliers than inliers is one the model contradicts, not one to average.
inline bool attitudeMajority(const UpConsensus& u) {
    return u.ok && 2 * u.outliers <= u.votes;
}

inline AttitudeFit fitAttitudeGauge(const Reconstruction& rec, const AttitudeRef& ref,
                                    bool north) {
    AttitudeFit fit;
    std::vector<Mat3> W, R;
    std::vector<Vec3> votes;
    for (size_t k = 0; k < ref.image_ids.size(); k++) {
        const auto it = rec.images.find(ref.image_ids[k]);
        if (it == rec.images.end() || !it->second.registered) continue;
        const Mat3& w = ref.world_from_cam[k];
        W.push_back(w);
        R.push_back(it->second.pose.R);
        // World +Z in the camera is the third row of camera -> world.
        votes.push_back(mul(transpose(R.back()), Vec3{w[6], w[7], w[8]}));
    }
    if (votes.size() < 3) return fit;
    fit.up = consensusUp(votes);
    if (!attitudeMajority(fit.up)) {
        fit.reason = AttitudeFail::Disagree;
        return fit;
    }
    const Mat3 level = rotationUpToZ(fit.up.up);
    Mat3 turn = mat3Identity();
    if (north) {
        std::vector<Vec3> hv;
        for (size_t k = 0; k < W.size(); k++) {
            // East-north-up from the levelled model: a turn about +Z, up to noise.
            const Mat3 E = mul(W[k], mul(R[k], transpose(level)));
            const double a = std::atan2(E[3] - E[1], E[0] + E[4]);
            hv.push_back({std::cos(a), std::sin(a), 0});
        }
        fit.heading = consensusUp(hv);
        if (attitudeMajority(fit.heading)) {
            const double c = fit.heading.up.x, s = fit.heading.up.y, n = std::hypot(c, s);
            turn = Mat3{c / n, -s / n, 0, s / n, c / n, 0, 0, 0, 1};
            fit.north = true;
        } else {
            fit.north_reason = AttitudeFail::Disagree;
        }
    }
    fit.T = normalizingTransform(rec, mul(turn, level));
    fit.ok = true;
    fit.reason = AttitudeFail::None;
    return fit;
}

}  // namespace sfm
