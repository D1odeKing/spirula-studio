// Local BA: a window solve (BundleOptions::only_points with held images) on
// every solver path, and the mapper's localRefine on a synthetic orbit.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "sfm/map/Bundle.h"
#include "sfm/map/Mapper.h"
#include "sfm/tests/SyntheticBA.h"
#include "sfm/tests/TestMain.h"

using namespace sfm;

static void require(bool v, const char* message) {
    if (!v) throw std::runtime_error(message);
}

static bool samePose(const Pose& a, const Pose& b) { return std::memcmp(&a, &b, sizeof(Pose)) == 0; }

// The solver's own synthetic problem as a reconstruction, with images 0-2 and,
// with `move_points`, the points 1000-1039 knocked off their true positions.
static Reconstruction fixture(uint32_t seed, bool move_points = true) {
    auto p = synth::makeProblem(5, 12, 160, 1, 0., seed, 0);
    Reconstruction rec;
    Camera cam = Camera::defaultFor(7, 640, 480, 600, CamModel::Pinhole);
    unpackIntrinsics(cam, p.intr.data());
    rec.cameras[7] = cam;
    for (uint32_t i = 0; i < p.num_images; ++i) {
        Image im;
        im.id = i;
        im.camera_id = 7;
        im.registered = true;
        im.pose = bundle_detail::unpackPose(&p.poses[6 * i]);
        rec.images[i] = im;
    }
    for (uint32_t k = 0; k < p.num_points; ++k) {
        Point3D point;
        point.xyz = {p.points[3 * k], p.points[3 * k + 1], p.points[3 * k + 2]};
        rec.points3D[k + 1000] = point;
    }
    for (uint32_t o = 0; o < p.num_obs; ++o) {
        auto& im = rec.images.at(p.obs_image[o]);
        const uint32_t f = uint32_t(im.points2D.size());
        const uint64_t id = p.obs_point[o] + 1000;
        im.points2D.push_back(cam.project(mul(im.pose.R, rec.points3D.at(id).xyz) + im.pose.t));
        im.point3D_ids.push_back(id);
        rec.points3D.at(id).track.push_back({im.id, f});
    }
    for (uint32_t i = 0; i < 3; ++i)
        rec.images.at(i).pose.t = rec.images.at(i).pose.t + Vec3{.02 * (i + 1), -.01, .015};
    for (uint64_t k = 1000; k < 1040 && move_points; ++k)
        rec.points3D.at(k).xyz = rec.points3D.at(k).xyz + Vec3{.03, -.02, .04};
    return rec;
}

// Points 1000-1079 form the window; the images seeing them other than 0-2 are held.
static void windowSolve() {
    for (RealCfg real : {RealCfg::CPU, RealCfg::F64})
        for (const char* solver : {"dense", "cg"}) {
            const Reconstruction original = fixture(31);
            Reconstruction rec = original;
            std::set<uint64_t> window;
            for (uint64_t k = 1000; k < 1080; ++k) window.insert(k);
            const std::set<uint32_t> moving{0, 1, 2};
            std::set<uint32_t> held;
            for (uint64_t k : window)
                for (const TrackElement& e : rec.points3D.at(k).track)
                    if (!moving.count(e.image_id)) held.insert(e.image_id);
            require(!held.empty(), "fixture window holds nothing");

            BundleOptions opt;
            opt.real = real;
            opt.solver = solver;
            opt.loss = "huber";
            opt.max_iters = 25;
            opt.refine_intrinsics = false;
            opt.only_points = &window;
            opt.fixed_images = &held;
            SolverStats stats;
            opt.stats = &stats;
            const BundleLayout layout = buildBundle(rec, opt);
            require(layout.P.num_points == window.size(), "window packed the wrong points");
            require(std::count(layout.P.fixed_frames.begin(), layout.P.fixed_frames.end(), 1u) ==
                        (long)held.size(), "held frames mismatched");
            for (const auto& g : layout.P.groups) require(g.n_intr == 0, "intrinsics left free");

            const double final_cost = runGlobalBA(rec, opt);
            require(std::isfinite(final_cost) && final_cost < stats.initial_cost * .02,
                    "window did not recover its perturbed poses");
            for (uint32_t id : held)
                require(samePose(rec.images.at(id).pose, original.images.at(id).pose),
                        "held image moved");
            for (const auto& kv : original.points3D)
                if (!window.count(kv.first))
                    require(std::memcmp(&rec.points3D.at(kv.first).xyz, &kv.second.xyz,
                                        sizeof(Vec3)) == 0, "point outside the window moved");
            double now[12] = {}, was[12] = {};
            packIntrinsics(rec.cameras.at(7), now);
            packIntrinsics(original.cameras.at(7), was);
            require(std::memcmp(now, was, sizeof now) == 0, "held intrinsics moved");
            require((rec.images.at(0).pose.t - original.images.at(0).pose.t).norm() > .01,
                    "moving image did not move");
            std::printf("PASS window real %d %s: %u img (%zu held) cost %.6g -> %.6g\n", int(real),
                        solver, layout.P.num_images, held.size(), stats.initial_cost, final_cost);
        }
}

// Tracks over `limit` views are held and seen from the moving images only.
static void clippedWindow() {
    const size_t limit = 6;
    for (RealCfg real : {RealCfg::CPU, RealCfg::F64}) {
        Reconstruction original = fixture(31, false);
        // Even points lose their views from images 6-11: short tracks beside long ones.
        for (auto& kv : original.points3D) {
            if (kv.first % 2) continue;
            auto& tr = kv.second.track;
            for (const TrackElement& e : tr)
                if (e.image_id >= 6) original.images.at(e.image_id).point3D_ids[e.point2D_idx] = kInvalidPoint3D;
            tr.erase(std::remove_if(tr.begin(), tr.end(),
                                    [](const TrackElement& e) { return e.image_id >= 6; }),
                     tr.end());
        }
        Reconstruction rec = original;
        const std::set<uint32_t> moving{0, 1, 2};
        std::set<uint64_t> window, held_pts;
        for (uint32_t v : moving)
            for (uint64_t id : rec.images.at(v).point3D_ids)
                if (id != kInvalidPoint3D) window.insert(id);
        std::set<uint32_t> held, present = moving;
        size_t expect_obs = 0;
        for (uint64_t k : window) {
            const auto& track = rec.points3D.at(k).track;
            if (track.size() > limit) held_pts.insert(k);
            for (const TrackElement& e : track) {
                if (track.size() > limit && !moving.count(e.image_id)) continue;
                expect_obs++;
                present.insert(e.image_id);
                if (!moving.count(e.image_id)) held.insert(e.image_id);
            }
        }
        require(!held_pts.empty() && held_pts.size() < window.size(), "fixture has no mix of tracks");
        BundleOptions opt;
        opt.real = real;
        opt.loss = "huber";
        opt.refine_intrinsics = false;
        opt.only_points = &window;
        opt.fixed_images = &held;
        opt.fixed_points = &held_pts;
        opt.window_images = &moving;
        opt.window_track = limit;
        SolverStats stats;
        opt.stats = &stats;
        const BundleLayout layout = buildBundle(rec, opt);
        require(layout.P.num_obs == expect_obs, "clipped window kept the wrong observations");
        require(layout.P.num_images == present.size(), "clipped window packed the wrong images");
        // The solver's own copy, not the write-back, which skips held points.
        {
            BundleLayout direct = buildBundle(rec, opt);
            BAProblem P = direct.P;
            const std::vector<double> before = P.points;
            solveBundle(P, bundleSolverOptions(opt), nullptr);
            double held_move = 0;
            for (uint32_t p = 0; p < P.num_points; p++)
                if (P.fixed_points[p])
                    for (int i = 0; i < 3; i++)
                        held_move = std::max(held_move, std::abs(P.points[3 * p + i] - before[3 * p + i]));
            std::printf("clipped window real %d: held points moved %.3g in the solver\n", int(real),
                        held_move);
            require(held_move == 0, "the solver moved a held point");
        }
        const double final_cost = runGlobalBA(rec, opt);
        require(final_cost < stats.initial_cost * .02, "clipped window did not recover its poses");
        for (uint64_t k : held_pts)
            require(std::memcmp(&rec.points3D.at(k).xyz, &original.points3D.at(k).xyz,
                                sizeof(Vec3)) == 0, "held long track moved");
        for (uint32_t id : held)
            require(samePose(rec.images.at(id).pose, original.images.at(id).pose), "held image moved");
        std::printf("PASS clipped window real %d: %u img, %u obs, %zu of %zu points held, cost "
                    "%.6g -> %.6g\n", int(real), layout.P.num_images, layout.P.num_obs,
                    held_pts.size(), window.size(), stats.initial_cost, final_cost);
    }
}

// A window over every point with nothing held is the global problem.
static void wholeWindow() {
    for (RealCfg real : {RealCfg::CPU, RealCfg::F64}) {
        Reconstruction a = fixture(47), b = a;
        std::set<uint64_t> all;
        for (const auto& kv : a.points3D) all.insert(kv.first);
        BundleOptions opt;
        opt.real = real;
        opt.solver = "dense";
        opt.loss = "huber";
        const double ca = runGlobalBA(a, opt);
        opt.only_points = &all;
        const double cb = runGlobalBA(b, opt);
        double worst = std::abs(ca - cb) / std::max(1.0, std::abs(ca));
        for (const auto& kv : a.images)
            worst = std::max(worst, (kv.second.pose.t - b.images.at(kv.first).pose.t).norm());
        for (const auto& kv : a.points3D)
            worst = std::max(worst, (kv.second.xyz - b.points3D.at(kv.first).xyz).norm());
        std::printf("whole window real %d: cost %.9g vs %.9g, worst difference %.3g\n", int(real),
                    ca, cb, worst);
        require(worst < 1e-6, "whole-model window differs from global BA");
    }
}

static Pose lookAt(const Vec3& C, const Vec3& target) {
    Vec3 f = (target - C).normalized();
    Vec3 r = Vec3{0, 1, 0}.cross(f).normalized();
    Vec3 u = f.cross(r);
    Mat3 R = {r.x, r.y, r.z, u.x, u.y, u.z, f.x, f.y, f.z};
    Vec3 t = mul(R, C);
    return {R, {-t.x, -t.y, -t.z}};
}

struct MapScore {
    uint32_t registered = 0;
    size_t models = 0;
    double rel_rot_deg = 0, reproj_px = 0;
};

// 48 cameras on a full orbit, every pair that shares 15 points matched;
// `outliers` of the keypoints moved 3-8 px, so the filters have work to do.
static MapScore mapOrbit(bool local_ba, int device, double outliers = 0,
                         const char* local_real = "cpu") {
    const int W = 1280, H = 960, M = 48, N = 400;
    const Camera K = Camera::defaultFor(1, W, H, 1200);
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> ub(-2.5, 2.5);
    std::normal_distribution<double> noise(0.0, 0.4);
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    std::vector<Vec3> pts(N);
    for (auto& p : pts) p = {ub(rng), ub(rng), ub(rng)};
    std::vector<Pose> gt(M);
    for (int c = 0; c < M; c++) {
        const double ang = 2 * M_PI * c / M;
        gt[c] = lookAt({9 * std::sin(ang), 1.5 * std::sin(0.7 * c), 9 * std::cos(ang)}, {0, 0, 0});
    }
    std::vector<FeatureSet> feats(M);
    std::vector<std::vector<char>> vis(M, std::vector<char>(N, 0));
    for (int c = 0; c < M; c++) {
        feats[c].width = W;
        feats[c].height = H;
        feats[c].keypoints.resize(N);
        for (int p = 0; p < N; p++) {
            const Vec3 pc = mul(gt[c].R, pts[p]) + gt[c].t;
            const Vec2 px = K.project(pc);
            if (pc.z > 0.1 && px.x > 0 && px.x < W && px.y > 0 && px.y < H) {
                double dx = noise(rng), dy = noise(rng);
                if (u01(rng) < outliers) {
                    const double r = 3 + 5 * u01(rng), a = 2 * M_PI * u01(rng);
                    dx += r * std::cos(a);
                    dy += r * std::sin(a);
                }
                feats[c].keypoints[p] = {(float)(px.x + dx), (float)(px.y + dy), 2, 0, 0};
                vis[c][p] = 1;
            } else {
                feats[c].keypoints[p] = {-1000, -1000, 2, 0, 0};
            }
        }
    }
    MatchesDatabase db;
    db.images.resize(M);
    for (int c = 0; c < M; c++) db.images[c] = {"cam" + std::to_string(c), (uint32_t)N};
    for (int i = 0; i < M; i++)
        for (int j = i + 1; j < M; j++) {
            TwoViewMatches tv;
            tv.image1 = i;
            tv.image2 = j;
            tv.config = (int)TwoViewConfig::Uncalibrated;
            for (int p = 0; p < N; p++)
                if (vis[i][p] && vis[j][p]) tv.matches.push_back({(uint32_t)p, (uint32_t)p});
            if (tv.matches.size() >= 15) db.pairs.push_back(std::move(tv));
        }

    MapperOptions opt;
    opt.verbose = false;
    opt.focal = 1200;
    opt.device = device;
    opt.local_ba = local_ba;
    opt.local_ba_real = local_real;
    Mapper mapper(db, feats, opt);
    const std::vector<Reconstruction> models = mapper.run();
    MapScore s;
    s.models = models.size();
    if (models.empty()) return s;
    const Reconstruction& rec = models.front();
    s.registered = rec.numRegistered();
    std::vector<uint32_t> reg;
    for (const auto& kv : rec.images)
        if (kv.second.registered) reg.push_back(kv.first);
    for (size_t k = 1; k < reg.size(); k++) {
        const Mat3& Ri = rec.images.at(reg[k - 1]).pose.R;
        const Mat3& Rj = rec.images.at(reg[k]).pose.R;
        const Mat3 est = mul(Rj, transpose(Ri));
        const Mat3 ref = mul(gt[reg[k]].R, transpose(gt[reg[k - 1]].R));
        s.rel_rot_deg = std::max(s.rel_rot_deg, rotationAngleDeg(mul(est, transpose(ref))));
    }
    double err = 0;
    size_t n = 0;
    for (const auto& kv : rec.points3D)
        for (const TrackElement& e : kv.second.track) {
            const Image& im = rec.images.at(e.image_id);
            const Vec2 px = rec.cameras.at(im.camera_id).project(mul(im.pose.R, kv.second.xyz) + im.pose.t);
            const Vec2& o = im.points2D[e.point2D_idx];
            err += std::hypot(px.x - o.x, px.y - o.y);
            n++;
        }
    s.reproj_px = n ? err / n : 0;
    return s;
}

static void mapper(int device) {
    const long before = g_map_prof.n_local_ba;
    const MapScore off = mapOrbit(false, device);
    require(g_map_prof.n_local_ba == before, "local BA ran with --local-ba off");
    const MapScore on = mapOrbit(true, device);
    const long ran = g_map_prof.n_local_ba - before;
    for (const MapScore* s : {&off, &on})
        std::printf("orbit local_ba=%d: %u/48 registered in %zu model(s), worst relative rotation "
                    "%.4f deg, mean reprojection %.3f px\n", s == &on, s->registered, s->models,
                    s->rel_rot_deg, s->reproj_px);
    std::printf("orbit: %ld local BA solve(s)\n", ran);
    require(ran > 0, "local BA never ran");
    require(on.registered == 48 && on.models == 1, "local BA lost registrations");
    require(on.rel_rot_deg < 0.2, "local BA left a rotation error");
    require(on.reproj_px < 1.0, "local BA left a reprojection error");
    require(on.rel_rot_deg < off.rel_rot_deg + 0.05, "local BA worse than global-only");

    // The score cache must track what the local filter removes; with
    // SS_SFM_SCORE_CHECK set, a stale entry aborts the ranking.
    const long filtered = g_map_prof.n_local_filtered;
    const MapScore noisy = mapOrbit(true, device, 0.08);
    std::printf("orbit with outliers: %u/48 registered, %ld obs filtered by local BA, worst "
                "relative rotation %.4f deg\n", noisy.registered,
                g_map_prof.n_local_filtered - filtered, noisy.rel_rot_deg);
    require(g_map_prof.n_local_filtered > filtered, "local filter removed nothing");
    require(noisy.registered == 48 && noisy.rel_rot_deg < 0.5, "outliers broke local BA");

    const MapScore dev = mapOrbit(true, device, 0.08, "double");
    std::printf("orbit with outliers, device local BA: %u/48 registered, worst relative rotation "
                "%.4f deg, mean reprojection %.3f px\n", dev.registered, dev.rel_rot_deg, dev.reproj_px);
    require(dev.registered == 48 && dev.models == 1 && dev.rel_rot_deg < 0.5,
            "device local BA broke the orbit");
}

// `sfm_local_ba_test MODEL_DIR`: windows cut from a real model the way
// Mapper::localRefine cuts them, solved on the host and on the device.
static int realWindows(const std::string& dir) {
    const Reconstruction model = Reconstruction::readBinary(dir);
    std::vector<uint32_t> ids;
    for (const auto& kv : model.images)
        if (kv.second.registered) ids.push_back(kv.first);
    VkContext shared;
    for (int k = 1; k <= 5; k++) {
        const uint32_t img = ids[ids.size() * k / 6];
        std::map<uint32_t, uint32_t> covis;
        for (uint64_t id : model.images.at(img).point3D_ids) {
            if (id == kInvalidPoint3D) continue;
            for (const TrackElement& e : model.points3D.at(id).track)
                if (e.image_id != img) covis[e.image_id]++;
        }
        std::vector<std::pair<uint32_t, uint32_t>> ranked(covis.begin(), covis.end());
        std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        });
        std::set<uint32_t> moving{img};
        for (size_t i = 0; i < ranked.size() && i < 6; i++) moving.insert(ranked[i].first);
        std::set<uint64_t> window, held_pts;
        std::set<uint32_t> held;
        for (uint32_t v : moving)
            for (uint64_t id : model.images.at(v).point3D_ids)
                if (id != kInvalidPoint3D) window.insert(id);
        for (uint64_t id : window) {
            const auto& tr = model.points3D.at(id).track;
            if (tr.size() > 15) { held_pts.insert(id); continue; }
            for (const TrackElement& e : tr)
                if (!moving.count(e.image_id)) held.insert(e.image_id);
        }
        Reconstruction start = model;
        start.images.at(img).pose.t = start.images.at(img).pose.t + Vec3{.01, -.01, .01};
        struct Run { const char* name; RealCfg real; bool shared; };
        std::vector<Reconstruction> out;
        for (const Run& r : {Run{"cpu", RealCfg::CPU, false}, Run{"f64", RealCfg::F64, false},
                             Run{"f64 shared ctx", RealCfg::F64, true}}) {
            Reconstruction rec = start;
            BundleOptions opt;
            opt.real = r.real;
            opt.loss = "huber";
            opt.refine_intrinsics = false;
            opt.only_points = &window;
            opt.fixed_images = &held;
            opt.fixed_points = &held_pts;
            opt.window_images = &moving;
            opt.window_track = 15;
            opt.shared_ctx = r.shared ? &shared : nullptr;
            BundleLayout layout = buildBundle(rec, opt);
            layout.attachPriors();
            BAProblem P = layout.P;
            const std::vector<double> pts0 = P.points;
            const BundleRun run = solveBundle(P, bundleSolverOptions(opt), opt.shared_ctx);
            const SolverStats& stats = run.stats;
            double held_move = 0, free_move = 0;
            for (uint32_t p = 0; p < P.num_points; p++) {
                const double d = std::hypot(std::hypot(P.points[3 * p] - pts0[3 * p],
                                                       P.points[3 * p + 1] - pts0[3 * p + 1]),
                                            P.points[3 * p + 2] - pts0[3 * p + 2]);
                double& m = !P.fixed_points.empty() && P.fixed_points[p] ? held_move : free_move;
                m = std::max(m, d);
            }
            require(held_move == 0, "a real window moved a held point");
            writeBundle(rec, layout, P);
            double moved = 0;
            for (uint32_t v : moving)
                moved = std::max(moved, (rec.images.at(v).pose.t - model.images.at(v).pose.t).norm());
            std::printf("window at %u, %s: %u img (%zu held) %u pt (%zu held) %u obs, %s, %d iters, "
                        "cost %.6g -> %.6g, pose move %.3g, held points %.3g, free points %.3g\n",
                        img, r.name, layout.P.num_images, held.size(), layout.P.num_points,
                        held_pts.size(), layout.P.num_obs, stats.solver, stats.iterations,
                        stats.initial_cost, stats.final_cost, moved, held_move, free_move);
            out.push_back(std::move(rec));
        }
        double worst = 0;
        for (size_t r = 1; r < out.size(); r++)
            for (uint32_t v : moving)
                worst = std::max(worst, (out[r].images.at(v).pose.t - out[0].images.at(v).pose.t).norm());
        std::printf("  device against host: worst pose difference %.3g\n", worst);
    }
    return 0;
}

static int test(int argc, char** argv) {
    if (argc == 1) return realWindows(argv[0]);
    int device = -1;
    for (int i = 0; i + 1 < argc; i++)
        if (std::string(argv[i]) == "--device") device = std::stoi(argv[i + 1]);
#ifdef _WIN32
    _putenv_s("SS_SFM_SCORE_CHECK", "1");
#else
    setenv("SS_SFM_SCORE_CHECK", "1", 1);
#endif
    windowSolve();
    clippedWindow();
    wholeWindow();
    mapper(device);
    std::printf("PASS\n");
    return 0;
}

int main(int argc, char** argv) { return sfmTestMain(argc - 1, argv + 1, test); }
