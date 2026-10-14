// Storage.cpp -- see Storage.h.

#include "app/gui/Storage.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/statvfs.h>
#endif

namespace fs = std::filesystem;

namespace gui {
namespace storage {

namespace {

std::string lower_ext(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return {};
    std::string e = name.substr(dot + 1);
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

// File systems that hold no user files, or only the kernel's view of itself.
bool pseudo_fs(const std::string& type) {
    static const char* kSkip[] = {
        "proc", "sysfs", "devtmpfs", "devpts", "cgroup", "cgroup2", "mqueue", "debugfs",
        "tracefs", "securityfs", "pstore", "bpf", "configfs", "fusectl", "hugetlbfs",
        "autofs", "binfmt_misc", "rpc_pipefs", "nsfs", "efivarfs", "selinuxfs", "ramfs",
        "squashfs", "fuse.lxcfs", "fuse.portal", "fuse.gvfsd-fuse",
    };
    for (const char* k : kSkip)
        if (type == k) return true;
    return false;
}

// A mount point the system or the container runtime put there, not a volume.
bool system_mount(const std::string& path) {
    static const char* kPrefixes[] = {"/proc", "/sys", "/dev", "/run", "/boot", "/snap",
                                      "/etc", "/usr", "/var/lib/docker"};
    if (path == "/dev/shm") return false;   // fast scratch space on a pod
    for (const char* p : kPrefixes) {
        const size_t n = std::strlen(p);
        if (path.compare(0, n, p) == 0 && (path.size() == n || path[n] == '/')) return true;
    }
    return false;
}

// /proc/self/mounts escapes a space as \040.
std::string unescape_mount(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 3 < s.size() && std::isdigit((unsigned char)s[i + 1])) {
            out += (char)std::strtol(s.substr(i + 1, 3).c_str(), nullptr, 8);
            i += 3;
        } else {
            out += s[i];
        }
    }
    return out;
}

void fill_space(Location& l) {
#ifdef _WIN32
    ULARGE_INTEGER avail{}, total{}, free_all{};
    if (GetDiskFreeSpaceExA(l.path.c_str(), &avail, &total, &free_all)) {
        l.total = total.QuadPart;
        l.free = avail.QuadPart;
    }
#else
    struct statvfs v{};
    if (statvfs(l.path.c_str(), &v) == 0) {
        l.total = (uint64_t)v.f_blocks * v.f_frsize;
        l.free = (uint64_t)v.f_bavail * v.f_frsize;
    }
#endif
}

std::string home_dir() {
#ifdef _WIN32
    const char* h = std::getenv("USERPROFILE");
#else
    const char* h = std::getenv("HOME");
#endif
    return h ? h : "";
}

}  // namespace

std::vector<Location> parse_mounts(const std::string& text) {
    std::vector<Location> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream f(line);
        std::string dev, path, type;
        if (!(f >> dev >> path >> type)) continue;
        path = unescape_mount(path);
        if (pseudo_fs(type) || system_mount(path)) continue;
        // Docker bind-mounts these three files from the host on every container.
        const std::string base = fs::path(path).filename().string();
        if (base == "hostname" || base == "hosts" || base == "resolv.conf") continue;
        if (std::any_of(out.begin(), out.end(), [&](const Location& l) { return l.path == path; }))
            continue;
        Location l;
        l.path = path;
        l.fs_type = type;
        l.label = path == "/" ? "/" : fs::path(path).filename().string();
        out.push_back(l);
    }
    std::stable_sort(out.begin(), out.end(), [](const Location& a, const Location& b) {
        return (a.path == "/") > (b.path == "/");
    });
    return out;
}

std::vector<Location> locations() {
    std::vector<Location> out;
#ifdef _WIN32
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(drives & (1u << i))) continue;
        Location l;
        l.path = std::string(1, (char)('A' + i)) + ":\\";
        l.label = l.path;
        out.push_back(l);
    }
#elif defined(__APPLE__)
    out.push_back({"/", "/", "", 0, 0});
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/Volumes", ec))
        out.push_back({e.path().string(), e.path().filename().string(), "", 0, 0});
#else
    std::ifstream f("/proc/self/mounts");
    std::stringstream ss;
    ss << f.rdbuf();
    out = parse_mounts(ss.str());
    if (out.empty()) out.push_back({"/", "/", "", 0, 0});
#endif
    const std::string home = home_dir();
    if (!home.empty() &&
        std::none_of(out.begin(), out.end(), [&](const Location& l) { return l.path == home; }))
        out.insert(out.begin() + std::min<size_t>(1, out.size()), {home, "~", "", 0, 0});
    for (Location& l : out) fill_space(l);
    return out;
}

Kind kind_of(const std::string& name) {
    const std::string e = lower_ext(name);
    static const char* kImage[] = {"jpg", "jpeg", "png", "bmp", "tga", "gif", "tif", "tiff",
                                   "exr", "hdr", "webp", "heic", "heif", "pgm", "ppm", "pnm"};
    static const char* kVideo[] = {"mp4", "mov", "m4v", "mkv", "avi", "webm", "insv", "360",
                                   "osv", "mts", "m2ts", "lrv"};
    static const char* kText[] = {"txt", "log", "json", "yaml", "yml", "xml", "csv", "tsv",
                                  "md", "ini", "cfg", "conf", "sh", "py", "cpp", "h", "c",
                                  "hpp", "js", "html", "css", "toml", "cube", "mtl", "out"};
    static const char* kCloud[] = {"e57", "las", "laz", "pcd", "xyz", "pts"};
    static const char* kMesh[] = {"obj", "glb", "gltf", "stl"};
    static const char* kArchive[] = {"zip", "tar", "gz", "tgz", "7z", "xz", "bz2"};
    auto in = [&](const char* const* list, size_t n) {
        for (size_t i = 0; i < n; i++)
            if (e == list[i]) return true;
        return false;
    };
    if (e == "ply" || e == "splat" || e == "spz" || e == "ksplat") return Kind::Splat;
    if (in(kImage, std::size(kImage))) return Kind::Image;
    if (in(kVideo, std::size(kVideo))) return Kind::Video;
    if (in(kText, std::size(kText))) return Kind::Text;
    if (in(kCloud, std::size(kCloud))) return Kind::PointCloud;
    if (in(kMesh, std::size(kMesh))) return Kind::Mesh;
    if (in(kArchive, std::size(kArchive))) return Kind::Archive;
    return Kind::Other;
}

bool list_dir(const std::string& dir, bool show_hidden, Sort by, bool descending,
              std::vector<Entry>& out, std::string& error) {
    out.clear();
    std::error_code ec;
    fs::directory_iterator it(fs::u8path(dir), fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    for (const fs::directory_entry& d : it) {
        Entry e;
        e.name = d.path().filename().u8string();
        if (!show_hidden && !e.name.empty() && e.name[0] == '.') continue;
        std::error_code ec2;
        e.is_link = d.is_symlink(ec2);
        e.is_dir = d.is_directory(ec2);
        if (ec2) continue;
        e.kind = e.is_dir ? Kind::Folder : kind_of(e.name);
        if (!e.is_dir) e.size = d.file_size(ec2);
        if (ec2) e.size = 0;
        const auto t = d.last_write_time(ec2);
        if (!ec2) {
            // file_clock to system_clock without C++20's clock_cast.
            const auto sys = std::chrono::time_point_cast<std::chrono::seconds>(
                t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            e.mtime = sys.time_since_epoch().count();
        }
        out.push_back(std::move(e));
    }
    auto less = [&](const Entry& a, const Entry& b) {
        switch (by) {
            case Sort::Size: if (a.size != b.size) return a.size < b.size; break;
            case Sort::Modified: if (a.mtime != b.mtime) return a.mtime < b.mtime; break;
            case Sort::Kind: if (a.kind != b.kind) return a.kind < b.kind; break;
            case Sort::Name: break;
        }
        // Case-insensitive, so "b.jpg" is not filed after "Z.jpg".
        std::string x = a.name, y = b.name;
        for (char& c : x) c = (char)std::tolower((unsigned char)c);
        for (char& c : y) c = (char)std::tolower((unsigned char)c);
        return x < y;
    };
    std::sort(out.begin(), out.end(), [&](const Entry& a, const Entry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir;
        return descending ? less(b, a) : less(a, b);
    });
    return true;
}

PlyInfo probe_ply(const std::string& path) {
    PlyInfo info;
    std::ifstream f(fs::u8path(path), std::ios::binary);
    std::string line;
    if (!std::getline(f, line) || line.rfind("ply", 0) != 0) return info;
    std::string element;
    bool has_dc = false, has_opacity = false;
    int rest = 0;
    for (int n = 0; n < 4096 && std::getline(f, line); n++) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ls(line);
        std::string word;
        ls >> word;
        if (word == "end_header") {
            info.ok = true;
            break;
        }
        if (word == "format") ls >> info.format;
        if (word == "element") {
            int64_t count = 0;
            ls >> element >> count;
            if (element == "vertex") info.vertices = count;
            if (element == "face") info.faces = count;
        }
        if (word == "property" && element == "vertex") {
            std::string type, name;
            ls >> type >> name;
            if (type == "list") ls >> name >> name;
            info.properties.push_back(name);
            if (name == "f_dc_0") has_dc = true;
            if (name == "opacity") has_opacity = true;
            if (name.rfind("f_rest_", 0) == 0) rest++;
        }
    }
    if (!info.ok) return info;
    if (has_dc && has_opacity) {
        info.kind = Kind::Splat;
        // f_rest_* carries 3 * ((d+1)^2 - 1) coefficients.
        const int per = rest / 3;
        info.sh_degree = 0;
        while ((info.sh_degree + 2) * (info.sh_degree + 2) - 1 <= per) info.sh_degree++;
    } else if (info.faces > 0) {
        info.kind = Kind::Mesh;
    } else {
        info.kind = Kind::PointCloud;
    }
    return info;
}

bool read_text(const std::string& path, size_t max_bytes, std::string& out, bool& binary,
               bool& truncated, std::string& error) {
    out.clear();
    binary = truncated = false;
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) {
        error = "cannot open the file";
        return false;
    }
    out.resize(max_bytes + 1);
    f.read(&out[0], (std::streamsize)out.size());
    out.resize((size_t)f.gcount());
    if (out.size() > max_bytes) {
        out.resize(max_bytes);
        truncated = true;
    }
    if (out.find('\0') != std::string::npos) {
        binary = true;
        return true;
    }
    // UTF-8 well-formedness, allowing a sequence cut off by the size limit.
    for (size_t i = 0; i < out.size();) {
        const unsigned char c = (unsigned char)out[i];
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len) { binary = true; break; }
        if (i + len > out.size()) {
            if (truncated) out.resize(i);
            else binary = true;
            break;
        }
        for (size_t k = 1; k < len; k++)
            if (((unsigned char)out[i + k] >> 6) != 2) { binary = true; break; }
        if (binary) break;
        i += len;
    }
    return true;
}

std::string join_inside(const std::string& dir, const std::string& relative) {
    if (relative.empty()) return {};
    const fs::path rel = fs::u8path(relative);
    if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory()) return {};
    for (const fs::path& part : rel)
        if (part == ".." || part == "." || part.empty()) return {};
    return (fs::u8path(dir) / rel).u8string();
}

std::string format_size(uint64_t bytes) {
    static const char* kUnit[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 5) {
        v /= 1024.0;
        u++;
    }
    char buf[32];
    if (u == 0) std::snprintf(buf, sizeof buf, "%llu B", (unsigned long long)bytes);
    else std::snprintf(buf, sizeof buf, v < 10 ? "%.2f %s" : v < 100 ? "%.1f %s" : "%.0f %s", v, kUnit[u]);
    return buf;
}

}  // namespace storage
}  // namespace gui
