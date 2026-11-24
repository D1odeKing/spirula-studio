#include "mesh/MeshEval.h"

#include "data/DatasetParser.h"
#include "data/Json.h"
#include "data/Knn.h"
#include "mesh/MeshDistance.h"
#include "mesh/MeshImport.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <thread>

namespace meshing {

namespace {

// The cloud shifted to its first point before narrowing to float, so a
// geo-referenced frame keeps its resolution in the kd-tree.
struct PointIndex {
    double origin[3] = {0, 0, 0};
    std::vector<float> pts;
    std::unique_ptr<knn::KdTree3> tree;
    explicit PointIndex(const std::vector<double>& xyz) {
        const size_t n = xyz.size() / 3;
        if (n) std::copy_n(xyz.data(), 3, origin);
        pts.resize(n * 3);
        for (size_t i = 0; i < n * 3; ++i) pts[i] = (float)(xyz[i] - origin[i % 3]);
        tree = std::make_unique<knn::KdTree3>(pts.data(), (int64_t)n);
    }
    double nearest(const double q[3], int32_t* idx = nullptr) const {
        const float f[3] = {(float)(q[0] - origin[0]), (float)(q[1] - origin[1]), (float)(q[2] - origin[2])};
        float d2 = 0;
        int32_t i = -1;
        if (!tree->query(f, -1, 1, &d2, &i)) return std::numeric_limits<double>::infinity();
        if (idx) *idx = i;
        return std::sqrt((double)d2);
    }
};

bool in_box(const EvalOptions& o, const double* p) {
    if (!o.crop) return true;
    for (int k = 0; k < 3; ++k)
        if (p[k] < o.crop_lo[k] || p[k] > o.crop_hi[k]) return false;
    return true;
}

double median_of(std::vector<double> v) {
    if (v.empty()) return 0;
    const size_t m = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + m, v.end());
    return v[m];
}

double mean_of(const std::vector<double>& v) {
    double s = 0;
    for (double x : v) s += x;
    return v.empty() ? 0 : s / v.size();
}

void estimate_normals(EvalReference& ref, const PointIndex& index, int threads) {
    const int64_t n = (int64_t)ref.points.size() / 3;
    ref.normals.assign((size_t)n * 3, 0.0f);
    parallel_for(n, threads, [&](int64_t b, int64_t e) {
        constexpr int K = 12;
        float d2[K];
        int32_t idx[K];
        for (int64_t i = b; i < e; ++i) {
            const float* q = &index.pts[(size_t)i * 3];
            const int got = index.tree->query(q, -1, K, d2, idx);
            if (got < 3) continue;
            double mu[3] = {0, 0, 0};
            for (int j = 0; j < got; ++j)
                for (int k = 0; k < 3; ++k) mu[k] += index.pts[(size_t)idx[j] * 3 + k] / got;
            double A[9] = {};
            for (int j = 0; j < got; ++j) {
                double d[3];
                for (int k = 0; k < 3; ++k) d[k] = index.pts[(size_t)idx[j] * 3 + k] - mu[k];
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c) A[r * 3 + c] += d[r] * d[c];
            }
            double nrm[3];
            smallest_eigenvector(A, nrm);
            for (int k = 0; k < 3; ++k) ref.normals[(size_t)i * 3 + k] = (float)nrm[k];
        }
    });
}

bool solve6(double A[36], double b[6], double x[6]) {
    for (int c = 0; c < 6; ++c) {
        int p = c;
        for (int r = c + 1; r < 6; ++r)
            if (std::fabs(A[r * 6 + c]) > std::fabs(A[p * 6 + c])) p = r;
        if (!(std::fabs(A[p * 6 + c]) > 1e-300)) return false;
        if (p != c) {
            for (int k = 0; k < 6; ++k) std::swap(A[c * 6 + k], A[p * 6 + k]);
            std::swap(b[c], b[p]);
        }
        for (int r = c + 1; r < 6; ++r) {
            const double f = A[r * 6 + c] / A[c * 6 + c];
            for (int k = c; k < 6; ++k) A[r * 6 + k] -= f * A[c * 6 + k];
            b[r] -= f * b[c];
        }
    }
    for (int r = 5; r >= 0; --r) {
        double s = b[r];
        for (int k = r + 1; k < 6; ++k) s -= A[r * 6 + k] * x[k];
        x[r] = s / A[r * 6 + r];
    }
    return true;
}

void affine_apply(const double M[12], const double p[3], double out[3]) {
    for (int r = 0; r < 3; ++r) out[r] = M[r * 4] * p[0] + M[r * 4 + 1] * p[1] + M[r * 4 + 2] * p[2] + M[r * 4 + 3];
}

}  // namespace

bool load_reference(const std::string& path, EvalReference& out, std::string& error) {
    out = EvalReference{};
    const bool ply = std::filesystem::path(path).extension() == ".ply";
    if ((ply && ply_is_mesh(path)) || (!ply && is_mesh_path(path))) {
        if (!read_mesh(path, out.mesh, error)) return false;
        if (!out.mesh.F.empty()) return true;
    }
    try {
        const ColmapPoints3D pts = read_ply_points(path);
        out.points = pts.xyz;
        out.mesh = MeshData{};
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    if (out.points.empty()) {
        error = path + ": no points";
        return false;
    }
    return true;
}

EvalResult evaluate_mesh(const MeshData& mesh, EvalReference& ref, const EvalOptions& opt) {
    EvalResult r;
    if (ref.is_mesh() && ref.points.empty()) sample_surface(ref.mesh, opt.samples, opt.seed + 1, ref.points);

    std::unique_ptr<TriangleBvh> ref_bvh;
    std::unique_ptr<PointIndex> ref_index;
    if (ref.is_mesh()) ref_bvh = std::make_unique<TriangleBvh>(ref.mesh);
    else ref_index = std::make_unique<PointIndex>(ref.points);
    auto to_ref = [&](const double q[3]) {
        return ref_bvh ? std::sqrt(std::max(0.0, ref_bvh->closest(q))) : ref_index->nearest(q);
    };

    std::vector<double> samples;
    sample_surface(mesh, opt.samples, opt.seed, samples);
    std::vector<double> acc_all(samples.size() / 3);
    parallel_for((int64_t)acc_all.size(), opt.threads, [&](int64_t b, int64_t e) {
        for (int64_t i = b; i < e; ++i) acc_all[(size_t)i] = to_ref(&samples[(size_t)i * 3]);
    });
    std::vector<double> acc;
    for (size_t i = 0; i < acc_all.size(); ++i)
        if (in_box(opt, &samples[i * 3]) && acc_all[i] <= opt.max_dist) acc.push_back(acc_all[i]);

    const TriangleBvh mesh_bvh(mesh);
    std::vector<int64_t> ref_keep;
    for (int64_t i = 0; i < (int64_t)ref.points.size() / 3; ++i)
        if (in_box(opt, &ref.points[(size_t)i * 3])) ref_keep.push_back(i);
    std::vector<double> comp(ref_keep.size());
    parallel_for((int64_t)comp.size(), opt.threads, [&](int64_t b, int64_t e) {
        for (int64_t i = b; i < e; ++i) {
            const double d2 = mesh_bvh.closest(&ref.points[(size_t)ref_keep[(size_t)i] * 3]);
            comp[(size_t)i] = d2 < 0 ? std::numeric_limits<double>::infinity() : std::sqrt(d2);
        }
    });
    std::vector<double> comp_cut;
    for (double d : comp) if (d <= opt.max_dist) comp_cut.push_back(d);

    r.mesh_points = (int64_t)acc.size();
    r.ref_points = (int64_t)comp_cut.size();
    r.acc_mean = mean_of(acc);
    r.acc_median = median_of(acc);
    r.comp_mean = mean_of(comp_cut);
    r.comp_median = median_of(comp_cut);
    r.chamfer = 0.5 * (r.acc_mean + r.comp_mean);
    // Precision and recall count every point in the box, not only the ones
    // under max_dist: a point past the cut is still a miss.
    int64_t n_acc = 0, n_comp = 0;
    for (size_t i = 0; i < acc_all.size(); ++i) n_acc += in_box(opt, &samples[i * 3]);
    n_comp = (int64_t)comp.size();
    for (double tau : opt.taus) {
        F1Row row;
        row.tau = tau;
        int64_t a = 0, c = 0;
        for (size_t i = 0; i < acc_all.size(); ++i) a += in_box(opt, &samples[i * 3]) && acc_all[i] < tau;
        for (double d : comp) c += d < tau;
        row.precision = n_acc ? (double)a / n_acc : 0;
        row.recall = n_comp ? (double)c / n_comp : 0;
        row.f1 = row.precision + row.recall > 0 ? 2 * row.precision * row.recall / (row.precision + row.recall) : 0;
        r.f1.push_back(row);
    }

    r.vertex_error.resize(mesh.V.size());
    parallel_for((int64_t)mesh.V.size(), opt.threads, [&](int64_t b, int64_t e) {
        for (int64_t i = b; i < e; ++i) {
            const double q[3] = {mesh.V[(size_t)i][0], mesh.V[(size_t)i][1], mesh.V[(size_t)i][2]};
            r.vertex_error[(size_t)i] = (float)to_ref(q);
        }
    });
    return r;
}

double icp_point_to_plane(const MeshData& mesh, EvalReference& ref, double M[12],
                          int iterations, int threads) {
    std::unique_ptr<TriangleBvh> ref_bvh;
    std::unique_ptr<PointIndex> ref_index;
    if (ref.is_mesh()) ref_bvh = std::make_unique<TriangleBvh>(ref.mesh);
    else {
        ref_index = std::make_unique<PointIndex>(ref.points);
        if (ref.normals.size() != ref.points.size()) estimate_normals(ref, *ref_index, threads);
    }
    const size_t nv = mesh.V.size();
    const size_t stride = std::max<size_t>(1, nv / 50000);
    std::vector<size_t> pick;
    for (size_t i = 0; i < nv; i += stride) pick.push_back(i);
    struct Pair { double p[3], q[3], n[3], d; };
    std::vector<Pair> pairs(pick.size());
    double rms = 0;
    for (int it = 0; it < iterations; ++it) {
        parallel_for((int64_t)pick.size(), threads, [&](int64_t b, int64_t e) {
            for (int64_t i = b; i < e; ++i) {
                Pair& pr = pairs[(size_t)i];
                const auto& v = mesh.V[pick[(size_t)i]];
                const double src[3] = {v[0], v[1], v[2]};
                affine_apply(M, src, pr.p);
                if (ref_bvh) {
                    int f = -1;
                    pr.d = std::sqrt(std::max(0.0, ref_bvh->closest(pr.p, pr.q, &f)));
                    ref_bvh->face_normal(f, pr.n);
                } else {
                    int32_t j = -1;
                    pr.d = ref_index->nearest(pr.p, &j);
                    for (int k = 0; k < 3; ++k) {
                        pr.q[k] = ref.points[(size_t)j * 3 + k];
                        pr.n[k] = ref.normals[(size_t)j * 3 + k];
                    }
                }
            }
        });
        std::vector<double> ds;
        ds.reserve(pairs.size());
        for (const Pair& pr : pairs) ds.push_back(pr.d);
        const double trim = 3.0 * std::max(median_of(ds), 1e-12);
        double A[36] = {}, b[6] = {}, sq = 0;
        int64_t used = 0;
        for (const Pair& pr : pairs) {
            if (!(pr.d <= trim)) continue;
            double J[6];
            J[0] = pr.p[1] * pr.n[2] - pr.p[2] * pr.n[1];
            J[1] = pr.p[2] * pr.n[0] - pr.p[0] * pr.n[2];
            J[2] = pr.p[0] * pr.n[1] - pr.p[1] * pr.n[0];
            J[3] = pr.n[0]; J[4] = pr.n[1]; J[5] = pr.n[2];
            const double res = (pr.p[0] - pr.q[0]) * pr.n[0] + (pr.p[1] - pr.q[1]) * pr.n[1] +
                               (pr.p[2] - pr.q[2]) * pr.n[2];
            for (int r = 0; r < 6; ++r) {
                for (int c = 0; c < 6; ++c) A[r * 6 + c] += J[r] * J[c];
                b[r] -= J[r] * res;
            }
            sq += res * res;
            ++used;
        }
        rms = used ? std::sqrt(sq / used) : 0;
        double x[6];
        if (used < 6 || !solve6(A, b, x)) break;
        // Rodrigues of the small rotation, then [dR | dt] composed onto M.
        const double th = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
        double dR[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        if (th > 1e-15) {
            const double k[3] = {x[0] / th, x[1] / th, x[2] / th};
            const double c = std::cos(th), s = std::sin(th), C = 1 - c;
            dR[0] = c + k[0] * k[0] * C;        dR[1] = k[0] * k[1] * C - k[2] * s; dR[2] = k[0] * k[2] * C + k[1] * s;
            dR[3] = k[1] * k[0] * C + k[2] * s; dR[4] = c + k[1] * k[1] * C;        dR[5] = k[1] * k[2] * C - k[0] * s;
            dR[6] = k[2] * k[0] * C - k[1] * s; dR[7] = k[2] * k[1] * C + k[0] * s; dR[8] = c + k[2] * k[2] * C;
        }
        double N[12];
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) {
                double acc = 0;
                for (int k = 0; k < 3; ++k) acc += dR[r * 3 + k] * M[k * 4 + c];
                N[r * 4 + c] = acc;
            }
            N[r * 4 + 3] += x[3 + r];
        }
        std::copy_n(N, 12, M);
        if (th < 1e-10 && std::sqrt(x[3] * x[3] + x[4] * x[4] + x[5] * x[5]) < 1e-10 * (1 + trim)) break;
    }
    return rms;
}

bool read_measurements(const std::string& path, std::vector<MeasurementPair>& out, std::string& error) {
    out.clear();
    JsonValue root;
    try {
        root = json_parse_file(path);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    const JsonValue* list = root.is_array() ? &root : root.find("pairs");
    if (!list || !list->is_array()) {
        error = path + ": expected a \"pairs\" array";
        return false;
    }
    auto vec3 = [](const JsonValue* v, double o[3]) {
        if (!v || !v->is_array() || v->arr.size() != 3) return false;
        for (int k = 0; k < 3; ++k) o[k] = v->arr[(size_t)k].as_double(0);
        return true;
    };
    for (const JsonValue& e : list->arr) {
        MeasurementPair p;
        if (const JsonValue* n = e.find("name")) p.name = n->as_string();
        const JsonValue* t = e.find("true_distance");
        if (!vec3(e.find("p1"), p.p1) || !vec3(e.find("p2"), p.p2) || !t) {
            error = path + ": each pair needs p1, p2 and true_distance";
            return false;
        }
        p.truth = t->as_double(0);
        if (p.name.empty()) p.name = std::to_string(out.size() + 1);
        out.push_back(p);
    }
    return true;
}

void measure_pairs(const MeshData& mesh, std::vector<MeasurementPair>& pairs) {
    const TriangleBvh bvh(mesh);
    for (MeasurementPair& p : pairs) {
        double a[3], b[3];
        std::copy_n(p.p1, 3, a);
        std::copy_n(p.p2, 3, b);
        p.measured = std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
        if (!bvh.empty()) {
            bvh.closest(p.p1, a);
            bvh.closest(p.p2, b);
        }
        p.snapped = std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
    }
}

std::array<unsigned char, 3> error_color(float e, float clip) {
    const float t = clip > 0 ? std::min(1.0f, std::max(0.0f, e / clip)) : 0.0f;
    // A four-segment ramp: blue, cyan, green, yellow, red.
    static const float stops[5][3] = {{0, 0, 1}, {0, 1, 1}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}};
    const float x = t * 4;
    const int i = std::min(3, (int)x);
    const float f = x - i;
    std::array<unsigned char, 3> c;
    for (int k = 0; k < 3; ++k)
        c[(size_t)k] = (unsigned char)std::lround(255 * (stops[i][k] * (1 - f) + stops[i + 1][k] * f));
    return c;
}

}  // namespace meshing
