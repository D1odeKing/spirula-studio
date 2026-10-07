// The recorded camera attitude: the XMP it is read from, the rotation its
// angles name, and the gauge fitted from it (map/AttitudeGauge.h).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "sfm/core/Attitude.h"
#include "sfm/map/AttitudeGauge.h"
#include "sfm/tests/TestMain.h"

using namespace sfm;

static int fails = 0;

static void check(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        fails++;
    }
}

namespace {

const char* kDjiXmp =
    "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF><rdf:Description\n"
    "   drone-dji:GimbalRollDegree=\"+180.00\"\n"
    "   drone-dji:GimbalYawDegree=\"+123.00\"\n"
    "   drone-dji:GimbalPitchDegree=\"-55.00\"\n"
    "   drone-dji:FlightYawDegree=\"-12.40\"/></rdf:RDF></x:xmpmeta>";

// SOI, an Exif APP1 whose IFD0 holds only Orientation, the XMP APP1, EOI.
void write_jpeg(const std::string& path, int orientation, const std::string& xmp) {
    std::vector<uint8_t> f{0xFF, 0xD8};
    auto app1 = [&f](const std::string& payload) {
        const size_t len = 2 + payload.size();
        f.insert(f.end(), {0xFF, 0xE1, (uint8_t)(len >> 8), (uint8_t)len});
        f.insert(f.end(), payload.begin(), payload.end());
    };
    const uint8_t tiff[] = {'I', 'I', 42, 0, 8, 0, 0, 0,           // header, IFD0 at 8
                            1, 0,                                  // one entry
                            0x12, 0x01, 3, 0, 1, 0, 0, 0,          // Orientation, SHORT x1
                            (uint8_t)orientation, 0, 0, 0,
                            0, 0, 0, 0};                           // no IFD1
    app1(std::string("Exif\0\0", 6) + std::string((const char*)tiff, sizeof tiff));
    app1(std::string("http://ns.adobe.com/xap/1.0/\0", 29) + xmp);
    f.insert(f.end(), {0xFF, 0xD9});
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write((const char*)f.data(), (std::streamsize)f.size());
}

double rotDeg(const Mat3& A, const Mat3& B) {
    const Mat3 D = mul(A, transpose(B));
    const double c = std::max(-1.0, std::min(1.0, (D[0] + D[4] + D[8] - 1.0) * 0.5));
    return std::acos(c) * 180.0 / M_PI;
}

bool close_to(const Vec3& a, const Vec3& b) { return (a - b).norm() < 1e-9; }

Vec3 column(const Mat3& M, int c) { return {M[c], M[3 + c], M[6 + c]}; }

Mat3 randomRotation(std::mt19937& rng) {
    std::normal_distribution<double> n(0.0, 1.0);
    return angleAxisToRotation(Vec3{n(rng), n(rng), n(rng)} * 2.0);
}

// A drone capture: `n` cameras over a 100 m field looking at pitch -45, -55
// turned upside down by the gimbal, or straight down, reconstructed in the
// gauge `G` (model = G(world)). `noise_deg` perturbs each recorded attitude.
struct Capture {
    Reconstruction rec;
    AttitudeRef ref;
};

Capture makeCapture(int n, const Sim3& G, double noise_deg, std::mt19937& rng) {
    std::uniform_real_distribution<double> u(-50.0, 50.0), yaw(-180.0, 180.0);
    std::normal_distribution<double> noise(0.0, noise_deg);
    Capture c;
    for (int i = 0; i < n; i++) {
        CameraAttitude a;
        a.valid = true;
        a.yaw_deg = yaw(rng);
        a.pitch_deg = i % 3 == 0 ? -45.0 : i % 3 == 1 ? -55.0 : -90.0;
        a.roll_deg = i % 3 == 1 ? 180.0 : 0.0;
        const Mat3 W = attitudeWorldFromCamera(a);
        const Vec3 centre{u(rng), u(rng), 90.0 + 0.1 * u(rng)};
        Image im;
        im.id = (uint32_t)i;
        im.registered = true;
        im.name = "DJI_" + std::to_string(i) + ".JPG";
        im.pose.R = mul(transpose(W), transpose(G.R));
        im.pose.t = mul(im.pose.R, transformPoint(G, centre)) * -1.0;
        c.rec.images[im.id] = im;
        CameraAttitude rec = a;
        rec.yaw_deg += noise(rng);
        rec.pitch_deg += noise(rng);
        rec.roll_deg += noise(rng);
        c.ref.image_ids.push_back(im.id);
        c.ref.world_from_cam.push_back(attitudeWorldFromCamera(rec));
        c.ref.registered++;
    }
    return c;
}

}  // namespace

static void check_xmp();
static void check_convention();
static void check_fit();

static int cmdAttitudeTest(int, char**) {
    check_xmp();
    check_convention();
    check_fit();
    if (fails == 0) std::printf("sfm_attitude_test: OK\n");
    return fails == 0 ? 0 : 1;
}

static void check_xmp() {
    CameraAttitude a = parseCameraAttitude(kDjiXmp);
    check(a.valid, "the DJI attributes parse");
    check(a.yaw_deg == 123.0 && a.pitch_deg == -55.0 && a.roll_deg == 180.0,
          "yaw, pitch and roll read back");
    a = parseCameraAttitude(
        "<drone-dji:GimbalYawDegree>12.5</drone-dji:GimbalYawDegree>"
        "<drone-dji:GimbalPitchDegree> -90 </drone-dji:GimbalPitchDegree>"
        "<drone-dji:GimbalRollDegree>0</drone-dji:GimbalRollDegree>");
    check(a.valid && a.yaw_deg == 12.5 && a.pitch_deg == -90.0, "the element form parses");
    check(!parseCameraAttitude("drone-dji:GimbalYawDegree=\"1\" drone-dji:GimbalPitchDegree=\"2\"")
               .valid,
          "a missing angle is no attitude");
    check(!parseCameraAttitude(" drone-dji:GimbalYawDegree=\"x\" drone-dji:GimbalPitchDegree=\"2\" "
                               "drone-dji:GimbalRollDegree=\"3\"")
               .valid,
          "a value that is not a number is no attitude");

    const std::string dir = "sfm_attitude_test.tmp.d";
    std::filesystem::create_directories(dir);
    write_jpeg(dir + "/a.jpg", 1, kDjiXmp);
    write_jpeg(dir + "/b.jpg", 6, kDjiXmp);
    write_jpeg(dir + "/c.jpg", 1, "<x:xmpmeta/>");
    a = readCameraAttitude(dir + "/a.jpg");
    check(a.valid && a.yaw_deg == 123.0, "the attitude reads back through a file");
    check(readExif(dir + "/b.jpg").orientation == 6, "the Exif APP1 is still found");
    check(!readCameraAttitude(dir + "/c.jpg").valid, "an XMP packet without one is no attitude");

    Reconstruction rec;
    for (const char* name : {"a.jpg", "b.jpg", "c.jpg"}) {
        Image im;
        im.id = (uint32_t)rec.images.size();
        im.registered = true;
        im.name = name;
        rec.images[im.id] = im;
    }
    check(attitudeRefFromImages(rec, dir, false).image_ids.size() == 2,
          "every image carrying an attitude is read");
    const AttitudeRef turned = attitudeRefFromImages(rec, dir, true);
    check(turned.image_ids.size() == 1 && turned.image_ids[0] == 0 && turned.registered == 3,
          "turned pixels leave out the image whose tag turned them");
    std::filesystem::remove_all(dir);
}

// Camera x right, y down, z forward, written in east-north-up.
static void check_convention() {
    auto W = [](double y, double p, double r) {
        CameraAttitude a;
        a.valid = true;
        a.yaw_deg = y;
        a.pitch_deg = p;
        a.roll_deg = r;
        return attitudeWorldFromCamera(a);
    };
    const Vec3 E{1, 0, 0}, N{0, 1, 0}, U{0, 0, 1};
    Mat3 R = W(0, 0, 0);
    check(close_to(column(R, 2), N) && close_to(column(R, 0), E) &&
              close_to(column(R, 1), U * -1.0),
          "level and facing north: forward is north, right is east, down is down");
    check(close_to(column(W(90, 0, 0), 2), E), "yaw 90 faces east");
    R = W(0, -90, 0);
    check(close_to(column(R, 2), U * -1.0) && close_to(column(R, 1), N * -1.0),
          "pitch -90 looks down, the top of the frame towards the heading");
    check(close_to(column(W(0, 0, 90), 0), U * -1.0), "roll 90 drops the right side");
    check(rotDeg(W(123, -55, 180), W(-57, -125, 0)) < 1e-5,
          "a gimbal rolled over is the same camera pitched past the nadir");
}

static void check_fit() {
    std::mt19937 rng(7);
    Sim3 G;
    G.scale = 0.013;
    G.R = randomRotation(rng);
    G.t = Vec3{0.3, -2.0, 5.0};

    Capture c = makeCapture(60, G, 0.5, rng);
    AttitudeFit fit = fitAttitudeGauge(c.rec, c.ref, true);
    check(fit.ok && fit.north, "a consistent capture fixes up and north");
    // The gauge undoes G's rotation: world = T.R model, model = G.R world.
    check(rotDeg(mul(fit.T.R, G.R), mat3Identity()) < 0.3, "... to within the noise");
    check(fit.up.spread_deg < 1.0 && fit.up.outliers == 0, "... and says the votes agree");

    fit = fitAttitudeGauge(c.rec, c.ref, false);
    const Vec3 up = mul(fit.T.R, mul(G.R, Vec3{0, 0, 1}));
    check(fit.ok && !fit.north && up.z > std::cos(0.3 * M_PI / 180.0), "`up` levels alone");

    // A quarter of the recorded attitudes belong to some other camera.
    Capture bad = c;
    for (size_t k = 0; k < bad.ref.world_from_cam.size(); k += 4)
        bad.ref.world_from_cam[k] = randomRotation(rng);
    fit = fitAttitudeGauge(bad.rec, bad.ref, true);
    check(fit.ok && fit.up.outliers > 0, "a minority of wrong attitudes is outvoted");
    check(rotDeg(mul(fit.T.R, G.R), mat3Identity()) < 0.5, "... and does not move the answer");

    // Most of them wrong: the model contradicts the set, and nothing is applied.
    for (size_t k = 0; k < bad.ref.world_from_cam.size(); k++)
        if (k % 4 != 1) bad.ref.world_from_cam[k] = randomRotation(rng);
    fit = fitAttitudeGauge(bad.rec, bad.ref, true);
    check(!fit.ok && fit.reason == AttitudeFail::Disagree, "a contradicted set is refused");

    // Pitch and roll right, yaw scattered: up stands, north does not.
    Capture compass = c;
    std::uniform_real_distribution<double> turn(-M_PI, M_PI);
    for (Mat3& w : compass.ref.world_from_cam) {
        const double a = turn(rng);
        w = mul(Mat3{std::cos(a), -std::sin(a), 0, std::sin(a), std::cos(a), 0, 0, 0, 1}, w);
    }
    fit = fitAttitudeGauge(compass.rec, compass.ref, true);
    check(fit.ok && !fit.north && fit.north_reason == AttitudeFail::Disagree,
          "scattered headings level the model and leave north unset");

    Capture two = c;
    two.ref.image_ids.resize(2);
    two.ref.world_from_cam.resize(2);
    fit = fitAttitudeGauge(two.rec, two.ref, true);
    check(!fit.ok && fit.reason == AttitudeFail::Few, "two images are too few to vote");
}

int main(int argc, char** argv) { return sfmTestMain(argc, argv, cmdAttitudeTest); }
