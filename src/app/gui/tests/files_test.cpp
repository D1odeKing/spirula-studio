// files -- the Files window's storage layer (Storage.h), folder downloads
// (TarWriter.h), and a remote session's file routes (RemoteFiles.h) over a real
// socket: the token and its cookie, Range reads, uploads, and the web viewer.

#include "app/gui/RemoteFiles.h"
#include "app/gui/Storage.h"
#include "app/gui/TarWriter.h"
#include "app/webviewer/HttpServer.h"

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
using namespace gui;

namespace {

int g_failures = 0;

void expect(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "ok  " : "BAD ", what.c_str());
    if (!ok) g_failures++;
}

void write(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << bytes;
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

// One request, the whole reply.
std::string request(int port, const std::string& text) {
    socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, (sockaddr*)&addr, sizeof addr) != 0) {
        close_socket(fd);
        return "";
    }
    send(fd, text.data(), (int)text.size(), 0);
    // Sent all it will: a body shorter than its Content-Length ends here.
#ifdef _WIN32
    shutdown(fd, SD_SEND);
#else
    shutdown(fd, SHUT_WR);
#endif
    std::string got;
    char buf[65536];
    for (;;) {
        const int n = (int)recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        got.append(buf, (size_t)n);
    }
    close_socket(fd);
    return got;
}

std::string body_of(const std::string& reply) {
    const size_t at = reply.find("\r\n\r\n");
    return at == std::string::npos ? "" : reply.substr(at + 4);
}

std::string enc(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') out += (char)c;
        else {
            char b[4];
            std::snprintf(b, sizeof b, "%%%02X", c);
            out += b;
        }
    }
    return out;
}

// name -> content of every file in a tar, long names included.
std::vector<std::pair<std::string, std::string>> untar(const std::string& t) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string long_name;
    for (size_t at = 0; at + 512 <= t.size();) {
        const char* h = t.data() + at;
        if (h[0] == 0) break;
        const uint64_t size = std::strtoull(std::string(h + 124, 11).c_str(), nullptr, 8);
        const char type = h[156];
        const std::string data = t.substr(at + 512, size);
        std::string name = long_name.empty() ? std::string(h, strnlen(h, 100)) : long_name;
        long_name.clear();
        if (type == 'L') long_name = data.substr(0, data.find('\0'));
        else if (type == '0') out.push_back({name, data});
        at += 512 + ((size + 511) / 512) * 512;
    }
    return out;
}

void test_mounts() {
    const std::string text =
        "overlay / overlay rw 0 0\n"
        "proc /proc proc rw 0 0\n"
        "tmpfs /dev tmpfs rw 0 0\n"
        "shm /dev/shm tmpfs rw 0 0\n"
        "/dev/vda1 /etc/hostname ext4 rw 0 0\n"
        "cgroup2 /sys/fs/cgroup cgroup2 ro 0 0\n"
        "volume /workspace fuse.geesefs rw 0 0\n"
        "/dev/sdb /mnt/my\\040data xfs rw 0 0\n";
    const auto l = storage::parse_mounts(text);
    std::vector<std::string> paths;
    for (const auto& x : l) paths.push_back(x.path);
    expect(paths == std::vector<std::string>({"/", "/dev/shm", "/workspace", "/mnt/my data"}),
           "mounts: root, /dev/shm, the volume and a spaced path; pseudo and system ones left out");
    expect(l.size() > 2 && l[2].fs_type == "fuse.geesefs" && l[2].label == "workspace",
           "a mount keeps its type and is labelled by its name");
}

void test_kinds(const fs::path& tmp) {
    using storage::Kind;
    expect(storage::kind_of("a.JPG") == Kind::Image && storage::kind_of("b.insv") == Kind::Video &&
               storage::kind_of("c.yaml") == Kind::Text && storage::kind_of("d.ply") == Kind::Splat &&
               storage::kind_of("e.e57") == Kind::PointCloud && storage::kind_of("f.glb") == Kind::Mesh &&
               storage::kind_of("g.tar") == Kind::Archive && storage::kind_of("h") == Kind::Other,
           "kinds by extension");

    write(tmp / "list/B.txt", "bb");
    write(tmp / "list/a.txt", "a");
    write(tmp / "list/c.txt", "cccc");
    write(tmp / "list/.hidden", "x");
    fs::create_directories(tmp / "list/zdir");
    std::vector<storage::Entry> e;
    std::string err;
    storage::list_dir((tmp / "list").string(), false, storage::Sort::Name, false, e, err);
    std::vector<std::string> names;
    for (const auto& x : e) names.push_back(x.name);
    expect(names == std::vector<std::string>({"zdir", "a.txt", "B.txt", "c.txt"}),
           "folders first, then files by name ignoring case, hidden left out");
    storage::list_dir((tmp / "list").string(), true, storage::Sort::Size, true, e, err);
    expect(e.size() == 5 && e[0].name == "zdir" && e[1].name == "c.txt" && e[1].size == 4,
           "size, largest first, and hidden shown when asked");
    expect(!storage::list_dir((tmp / "nope").string(), false, storage::Sort::Name, false, e, err) &&
               !err.empty(),
           "a missing folder is an error");
}

void test_ply(const fs::path& tmp) {
    std::string splat = "ply\nformat binary_little_endian 1.0\nelement vertex 7\n";
    for (const char* p : {"x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2"})
        splat += std::string("property float ") + p + "\n";
    for (int i = 0; i < 24; i++) splat += "property float f_rest_" + std::to_string(i) + "\n";
    splat += "property float opacity\nend_header\n" + std::string(100, '\0');
    write(tmp / "s.ply", splat);
    const auto s = storage::probe_ply((tmp / "s.ply").string());
    expect(s.ok && s.kind == storage::Kind::Splat && s.vertices == 7 && s.sh_degree == 2,
           "a splat PLY: 7 splats, SH degree 2");
    write(tmp / "m.ply", "ply\r\nformat ascii 1.0\r\nelement vertex 3\r\nproperty float x\r\n"
                         "element face 1\r\nproperty list uchar int vertex_indices\r\nend_header\r\n");
    const auto m = storage::probe_ply((tmp / "m.ply").string());
    expect(m.ok && m.kind == storage::Kind::Mesh && m.faces == 1 && m.format == "ascii",
           "a mesh PLY, CRLF header");
    write(tmp / "p.ply", "ply\nformat ascii 1.0\nelement vertex 2\nproperty float x\nend_header\n");
    expect(storage::probe_ply((tmp / "p.ply").string()).kind == storage::Kind::PointCloud,
           "a bare point cloud PLY");
    write(tmp / "n.ply", "not a ply");
    expect(!storage::probe_ply((tmp / "n.ply").string()).ok, "not a PLY");
}

void test_text(const fs::path& tmp) {
    std::string out, err;
    bool binary = false, truncated = false;
    write(tmp / "t.txt", "h\xc3\xa9llo\n");
    storage::read_text((tmp / "t.txt").string(), 100, out, binary, truncated, err);
    expect(!binary && !truncated && out == "h\xc3\xa9llo\n", "UTF-8 text reads whole");
    storage::read_text((tmp / "t.txt").string(), 2, out, binary, truncated, err);
    expect(!binary && truncated && out == "h", "a cut through a character drops it, not the file");
    write(tmp / "b.bin", std::string("ab\0cd", 5));
    storage::read_text((tmp / "b.bin").string(), 100, out, binary, truncated, err);
    expect(binary, "a NUL makes it binary");

    expect(storage::join_inside("/data", "a/b.jpg") == (fs::path("/data") / "a/b.jpg").string(),
           "a relative name lands inside");
    expect(storage::join_inside("/data", "../etc/passwd").empty() &&
               storage::join_inside("/data", "/etc/passwd").empty() &&
               storage::join_inside("/data", "a/../../x").empty() &&
               storage::join_inside("/data", "").empty(),
           "climbing out, absolute and empty names are refused");
    expect(storage::format_size(512) == "512 B" && storage::format_size(1536) == "1.50 KB" &&
               storage::format_size(5ull << 30) == "5.00 GB",
           "sizes");
}

void test_tar(const fs::path& tmp) {
    const std::string long_dir(120, 'd');
    write(tmp / "tree/top.txt", "top");
    write(tmp / "tree" / long_dir / "deep.bin", std::string(1000, 'x'));
    fs::create_directories(tmp / "tree/empty");
    std::string t;
    write_tar((tmp / "tree").string(), [&](const void* d, size_t n) {
        t.append((const char*)d, n);
        return true;
    });
    const auto files = untar(t);
    bool top = false, deep = false;
    for (const auto& [name, data] : files) {
        top |= name == "tree/top.txt" && data == "top";
        deep |= name == "tree/" + long_dir + "/deep.bin" && data == std::string(1000, 'x');
    }
    expect(files.size() == 2 && top && deep, "a folder tars with its files, long names whole");
    expect(t.size() % 512 == 0 && t.compare(t.size() - 1024, 1024, std::string(1024, '\0')) == 0,
           "and ends in two empty blocks");
    int calls = 0;
    expect(!write_tar((tmp / "tree").string(), [&](const void*, size_t) { return ++calls < 2; }),
           "a reader that goes away stops it");
}

void test_range() {
    uint64_t a = 0, b = 0;
    expect(remote::parse_range("bytes=0-99", 1000, a, b) && a == 0 && b == 99, "range a-b");
    expect(remote::parse_range("bytes=900-", 1000, a, b) && a == 900 && b == 999, "range a-");
    expect(remote::parse_range("bytes=-100", 1000, a, b) && a == 900 && b == 999, "range -n");
    expect(remote::parse_range("bytes=500-5000", 1000, a, b) && b == 999, "range clamped");
    expect(!remote::parse_range("bytes=1000-", 1000, a, b) &&
               !remote::parse_range("bytes=0-1,5-9", 1000, a, b) &&
               !remote::parse_range("items=0-1", 1000, a, b),
           "past the end, several ranges and other units refused");
    expect(std::string(remote::mime_type("x.WASM")) == "application/wasm" &&
               std::string(remote::mime_type("a.mp4")) == "video/mp4" &&
               std::string(remote::mime_type("a.ply")) == "application/octet-stream",
           "content types");
}

void test_routes(const fs::path& tmp) {
    HttpServer http;
    http.set_token("tok");
    http.set_cookie_auth(true);
    int uploads = 0;
    remote::add_file_routes(http, [&] { uploads++; });
    int port = 0;
    for (int p = 18831; p < 18900 && !port; p++) {
        try {
            http.start("127.0.0.1", p);
            port = p;
        } catch (...) {}
    }
    expect(port != 0, "server binds");
    if (!port) return;
    const std::string cookie = "Cookie: other=1; " + http.cookie_name() + "=tok\r\n";

    const std::string page = request(port, "GET /viewer/index.html HTTP/1.0\r\n\r\n");
    expect(page.find(" 403 ") != std::string::npos, "the viewer wants the token");
    const std::string viewer = request(port, "GET /viewer/index.html HTTP/1.0\r\n" + cookie + "\r\n");
    expect(viewer.find(" 200 ") != std::string::npos && viewer.find("<html") != std::string::npos,
           "the cookie stands in for the token, and the viewer page is served");
    const std::string wasm = request(port, "GET /viewer/js/ssv_wasm.wasm HTTP/1.0\r\n" + cookie + "\r\n");
    expect(wasm.find("Content-Type: application/wasm") != std::string::npos &&
               body_of(wasm).compare(0, 4, std::string("\0asm", 4)) == 0,
           "its WebAssembly, with the type the browser insists on");

    std::string data(10000, '\0');
    for (size_t i = 0; i < data.size(); i++) data[i] = (char)(i * 7);
    write(tmp / "srv/v.mp4", data);
    const std::string raw_url = "/remote/raw/v.mp4?token=tok&path=" + enc((tmp / "srv/v.mp4").string());
    const std::string ranged =
        request(port, "GET " + raw_url + " HTTP/1.0\r\nRange: bytes=100-199\r\n\r\n");
    expect(ranged.find(" 206 ") != std::string::npos &&
               ranged.find("Content-Range: bytes 100-199/10000") != std::string::npos &&
               body_of(ranged) == data.substr(100, 100),
           "a Range read: 206 and exactly those bytes");
    const std::string whole = request(port, "GET " + raw_url + " HTTP/1.0\r\n\r\n");
    expect(whole.find(" 200 ") != std::string::npos && body_of(whole) == data &&
               whole.find("Content-Type: video/mp4") != std::string::npos,
           "the whole file, typed for a <video>");

    write(tmp / "srv/folder/x.txt", "xx");
    const std::string dl = request(port, "GET /remote/download/folder?token=tok&path=" +
                                             enc((tmp / "srv/folder").string()) + " HTTP/1.0\r\n\r\n");
    const auto files = untar(body_of(dl));
    expect(dl.find("attachment; filename=\"folder.tar\"") != std::string::npos && files.size() == 1 &&
               files[0].first == "folder/x.txt" && files[0].second == "xx",
           "a folder downloads as a tar");

    const std::string body = "uploaded bytes";
    auto upload = [&](const std::string& name, bool overwrite) {
        return request(port, "POST /remote/upload?dir=" + enc((tmp / "srv").string()) + "&name=" +
                                 enc(name) + (overwrite ? "&overwrite=1" : "") +
                                 " HTTP/1.0\r\n" + cookie + "Content-Length: " +
                                 std::to_string(body.size()) + "\r\n\r\n" + body);
    };
    expect(upload("sub/dir/u.txt", false).find(" 200 ") != std::string::npos &&
               slurp(tmp / "srv/sub/dir/u.txt") == body && uploads == 1,
           "an upload lands in its folder, the folders made on the way");
    expect(upload("sub/dir/u.txt", false).find(" 409 ") != std::string::npos,
           "and does not replace a file unasked");
    expect(upload("sub/dir/u.txt", true).find(" 200 ") != std::string::npos && uploads == 2,
           "unless asked to");
    expect(upload("../escape.txt", false).find(" 400 ") != std::string::npos &&
               !fs::exists(tmp / "escape.txt"),
           "a name that climbs out of the folder is refused");
    const std::string cut = request(port, "POST /remote/upload?dir=" + enc((tmp / "srv").string()) +
                                              "&name=cut.bin HTTP/1.0\r\n" + cookie +
                                              "Content-Length: 1000\r\n\r\nonly a little");
    bool leftovers = false;
    for (const auto& e : fs::directory_iterator(tmp / "srv"))
        leftovers |= e.path().filename().string().find(".part-") != std::string::npos;
    expect(cut.find(" 400 ") != std::string::npos && !fs::exists(tmp / "srv/cut.bin") && !leftovers,
           "an upload cut short leaves nothing behind");
    http.stop();
}

}  // namespace

int main() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    const fs::path tmp = fs::temp_directory_path() / "ss_files_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    test_mounts();
    test_kinds(tmp);
    test_ply(tmp);
    test_text(tmp);
    test_tar(tmp);
    test_range();
    test_routes(tmp);
    fs::remove_all(tmp);
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
