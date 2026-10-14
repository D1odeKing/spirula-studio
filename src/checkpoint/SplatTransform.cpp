// SplatTransform.cpp -- see SplatTransform.h.

#include "checkpoint/SplatTransform.h"

#include "checkpoint/SplatPly.h"
#include "core/SphericalBeta.h"

#include <cmath>

namespace spirula {

SplatTransform::SplatTransform(const Sim3& T, const ColorBasis& basis)
    : _T(T), _log_s((float)std::log(T.s)), _identity(T.is_identity()),
      _basis(basis), _sh(T.R, basis.sh_degree) {
    _T.quat(_q);
}

void SplatTransform::apply(float mean[3], float quat[4], float log_scale[3],
                           float* rest) const {
    if (_identity) return;
    const double p[3] = {mean[0], mean[1], mean[2]};
    double o[3];
    _T.apply(p, o);
    for (int i = 0; i < 3; i++) mean[i] = (float)o[i];

    // Hamilton product q_R * q: the Gaussian's own frame, then the turn.
    const double aw = _q[0], ax = _q[1], ay = _q[2], az = _q[3];
    const double bw = quat[0], bx = quat[1], by = quat[2], bz = quat[3];
    double r[4] = {aw*bw - ax*bx - ay*by - az*bz,
                   aw*bx + ax*bw + ay*bz - az*by,
                   aw*by - ax*bz + ay*bw + az*bx,
                   aw*bz + ax*by - ay*bx + az*bw};
    // The stored quaternion is not normalized, and its length is the file's
    // to keep: renormalizing here would be a second edit nobody asked for.
    for (int i = 0; i < 4; i++) quat[i] = (float)r[i];

    for (int i = 0; i < 3; i++) log_scale[i] += _log_s;
    if (!rest) return;
    const int sh = _basis.sh_coeffs();
    if (sh > 0) _sh.apply(rest, sh);
    for (int l = 0; l < _basis.sb_lobes; l++)
        sb::rotate_lobe(_T.R, rest + 3 * sh + sb::kLobeFloats * l);
}

void transform_splats(SplatCloud& c, const Sim3& T) {
    const SplatTransform xf(T, c.basis());
    if (xf.is_identity()) return;
    const int64_t K = c.coeffs();
#pragma omp parallel for schedule(static)
    for (int64_t i = 0; i < c.num; i++)
        xf.apply(&c.means[(size_t)i * 3], &c.quats[(size_t)i * 4],
                 &c.scales[(size_t)i * 3],
                 K > 0 ? &c.features_sh[(size_t)(i * K * 3)] : nullptr);
}

}  // namespace spirula
