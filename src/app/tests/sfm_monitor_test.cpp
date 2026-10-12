// sfm_monitor -- app/SfmMonitor.h over a real socket. The fixture is written
// with sfm::progress itself, so a change to a snapshot format that the monitor
// does not follow fails here rather than as a blank page.

#include "app/SfmMonitor.h"
#include "sfm/core/Events.h"
#include "sfm/core/Model.h"
#include "sfm/core/Progress.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET socket_t;
  static void close_socket(socket_t s) { closesocket(s); }
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  typedef int socket_t;
  static void close_socket(socket_t s) { ::close(s); }
#endif

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void expect(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "ok  " : "BAD ", what.c_str());
    if (!ok) g_failures++;
}

struct Reply {
    int status = 0;
    std::string body;
};

// One HTTP/1.0 GET; the server closes the connection after the body.
Reply get(int port, const std::string& target) {
    Reply r;
    socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (sockaddr*)&addr, sizeof addr) != 0) {
        close_socket(fd);
        return r;
    }
    const std::string req = "GET " + target + " HTTP/1.0\r\n\r\n";
    send(fd, req.data(), (int)req.size(), 0);
    std::string all;
    char buf[4096];
    for (;;) {
        const int n = (int)recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        all.append(buf, (size_t)n);
    }
    close_socket(fd);
    const size_t sp = all.find(' ');
    const size_t head = all.find("\r\n\r\n");
    if (sp == std::string::npos || head == std::string::npos) return r;
    r.status = std::atoi(all.c_str() + sp + 1);
    r.body = all.substr(head + 4);
    return r;
}

uint32_t u32_at(const std::string& b, size_t at) {
    uint32_t v = 0;
    if (at + 4 <= b.size()) std::memcpy(&v, b.data() + at, 4);
    return v;
}

size_t count(const std::string& s, const std::string& what) {
    size_t n = 0;
    for (size_t p = s.find(what); p != std::string::npos; p = s.find(what, p + 1)) n++;
    return n;
}

// Two placed images and one the mapper could not place, four points in front of
// both cameras, and a pair matrix with one cell verified empty.
void write_fixture(const fs::path& ws) {
    using namespace sfm;
    const fs::path pdir = ws / ".progress";
    progress::set_dir(pdir.string());
    progress::images({"a.jpg", "b.jpg", "c.jpg"}, {100, 200, 300});

    Reconstruction rec;
    Camera cam;
    cam.id = 1;
    cam.width = 640;
    cam.height = 480;
    cam.model = CamModel::Pinhole;
    cam.fx = cam.fy = 500;
    cam.cx = 320;
    cam.cy = 240;
    rec.cameras[1] = cam;
    const Vec3 pts[4] = {{0, 0, 5}, {1, 0, 5}, {0, 1, 5}, {1, 1, 6}};
    for (uint64_t p = 0; p < 4; p++) rec.points3D[p + 1].xyz = pts[p];
    for (uint32_t i = 0; i < 2; i++) {
        Image im;
        im.id = i;
        im.camera_id = 1;
        im.name = i ? "b.jpg" : "a.jpg";
        im.registered = true;
        im.pose.R = Mat3{1, 0, 0, 0, 1, 0, 0, 0, 1};
        im.pose.t = Vec3{-(double)i, 0, 0};
        for (uint64_t p = 0; p < 4; p++) {
            const Vec3 c = pts[p] + im.pose.t;
            // Image 1 sees its points a pixel off, so the two fit differently.
            im.points2D.push_back(Vec2{500 * c.x / c.z + 320 + i, 500 * c.y / c.z + 240});
            im.point3D_ids.push_back(p + 1);
            rec.points3D[p + 1].track.push_back({i, (uint32_t)p});
        }
        rec.images[i] = im;
    }
    // The mapper's model holds every image of the capture, placed or not.
    Image unplaced;
    unplaced.id = 2;
    unplaced.camera_id = 1;
    unplaced.name = "c.jpg";
    rec.images[2] = unplaced;
    progress::model(rec, /*force=*/true);

    progress::begin_matching(3, {{0, 1}, {0, 2}, {1, 2}});
    progress::pair(0, 1, 50);
    progress::pair(0, 2, 0);

    Event e;
    e.kind = Event::Kind::StageBegin;
    e.stage = Stage::Map;
    progress::status(e);
    e.kind = Event::Kind::Result;
    e.registered = 2;
    e.images = 3;
    e.points = 4;
    e.models = 1;
    e.mean_reproj = 0.5;
    progress::status(e);

    std::vector<uint8_t> rgb(8 * 8 * 3, 200);
    progress::thumbnail("a", rgb.data(), 8, 8);
    progress::flush();

    std::ofstream(ws / "progressive.txt") << "attempt 1: 2 of 3 placed\n";
}

}  // namespace

int main() {
    const fs::path ws = fs::absolute("sfm_monitor_test.tmp");
    std::error_code ec;
    fs::remove_all(ws, ec);
    fs::create_directories(ws);
    write_fixture(ws);

    app::SfmMonitorOptions opt;
    opt.progress_dir = (ws / ".progress").string();
    opt.workspace = ws.string();
    opt.log = true;
    app::SfmMonitor mon;
    std::string token;
    int port = 0;
    for (int p = 17608; p < 17640 && token.empty(); p++) {
        opt.port = p;
        try {
            token = mon.start(opt);
            port = p;
        } catch (const std::exception&) {
        }
    }
    expect(!token.empty() && token.size() == 32, "starts with a generated 32-digit token");
    if (token.empty()) return 1;
    mon.log("warning", "[map] WARNING something to see");
    const std::string t = "token=" + token;

    expect(get(port, "/status").status == 403, "no token: 403");
    std::string wrong = token;
    wrong[0] = wrong[0] == 'a' ? 'b' : 'a';
    expect(get(port, "/status?token=" + wrong).status == 403, "a wrong token of the right length: 403");
    expect(get(port, "/?" + t).body.find("<title>") != std::string::npos, "the page is served");

    const Reply st = get(port, "/status?" + t);
    expect(st.status == 200, "/status: 200");
    expect(st.body.find("\"have\":true") != std::string::npos, "/status: has a status");
    expect(st.body.find("\"finished\":true") != std::string::npos, "/status: the result's flag");
    expect(st.body.find("\"registered\":2") != std::string::npos, "/status: registered");
    expect(st.body.find("\"images\":3") != std::string::npos, "/status: images");
    expect(st.body.find("\"live\":true") != std::string::npos, "/status: the log is live");
    expect(st.body.find("\"log\":1") != std::string::npos, "/status: one log line");

    const Reply im = get(port, "/images?" + t);
    expect(im.status == 200, "/images: 200");
    expect(count(im.body, "\"id\":") == 3, "/images: three images, the unplaced one too");
    expect(count(im.body, "\"placed\":false") == 1, "/images: one unplaced");
    expect(im.body.find("\"bad_err\":") != std::string::npos, "/images: the fit scale");

    const Reply md = get(port, "/model?" + t);
    expect(md.status == 200 && md.body.compare(0, 4, "SSMV") == 0, "/model: magic");
    expect(u32_at(md.body, 8) == 2, "/model: two cameras");
    expect(u32_at(md.body, 12) == 4, "/model: four points");
    expect(u32_at(md.body, 16) == 3, "/model: three images in the capture");
    expect(md.body.size() == 48 + 2 * 92 + 4 * 15, "/model: the layout the page parses");
    expect(get(port, "/model?k=5&" + t).status == 404, "/model: a model that does not exist");

    const Reply mo = get(port, "/models?" + t);
    expect(mo.body == "{\"sizes\":[2]}", "/models: one model of two images");

    const Reply pr = get(port, "/pairs?" + t);
    expect(pr.status == 200 && pr.body.compare(0, 4, "SSPM") == 0, "/pairs: magic");
    expect(u32_at(pr.body, 8) == 3 && u32_at(pr.body, 16) == 1, "/pairs: three bins, staged");
    expect(pr.body.size() == 20 + 3 * 9 * 4, "/pairs: three planes");

    const Reply lg = get(port, "/log?since=0&" + t);
    expect(lg.body.find("something to see") != std::string::npos, "/log: the line");
    expect(lg.body.find("\"level\":\"warning\"") != std::string::npos, "/log: its level");
    expect(get(port, "/log?since=1&" + t).body.find("\"lines\":[]") != std::string::npos,
           "/log: nothing after the last line");

    expect(get(port, "/progressive?" + t).body.find("attempt 1") != std::string::npos,
           "/progressive: the workspace's report");

    const Reply th = get(port, "/thumb?name=a&" + t);
    expect(th.status == 200 && th.body.size() > 2 && (uint8_t)th.body[0] == 0xFF &&
               (uint8_t)th.body[1] == 0xD8,
           "/thumb: a JPEG");
    expect(get(port, "/thumb?name=a.jpg&" + t).status == 200, "/thumb: by the image's name");
    expect(get(port, "/thumb?name=../status.bin&" + t).status == 400, "/thumb: .. refused");
    expect(get(port, "/thumb?name=/etc/passwd&" + t).status == 400, "/thumb: absolute refused");
    expect(get(port, "/thumb?name=C:/x&" + t).status == 400, "/thumb: a drive refused");

    mon.stop();
    sfm::progress::set_dir("");
    fs::remove_all(ws, ec);
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
