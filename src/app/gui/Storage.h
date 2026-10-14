#pragma once

// What the Files window shows: the storage this machine has mounted, a
// folder's entries, what kind of file each is, and the cheap looks inside one
// (a PLY's header, the start of a text file). No GUI here; FileBrowser.h draws
// it. docs/notes/file-browser.md.

#include <cstdint>
#include <string>
#include <vector>

namespace gui {
namespace storage {

struct Location {
    std::string path;      // where it is mounted
    std::string label;     // what to call it
    std::string fs_type;   // "ext4", "fuse.geesefs", "tmpfs", ...; empty off Linux
    uint64_t total = 0, free = 0;   // bytes; 0 when unknown
};

// Every mounted file system worth browsing (pseudo file systems such as proc,
// sysfs and cgroup left out), plus the home folder, root first.
std::vector<Location> locations();

// The same from /proc/self/mounts text, without statting anything.
std::vector<Location> parse_mounts(const std::string& mounts_text);

enum class Kind { Folder, Image, Video, Text, Splat, PointCloud, Mesh, Archive, Other };

struct Entry {
    std::string name;
    Kind kind = Kind::Other;
    bool is_dir = false;
    bool is_link = false;
    uint64_t size = 0;      // bytes; 0 for a folder
    int64_t mtime = 0;      // seconds since the epoch
};

enum class Sort { Name, Size, Modified, Kind };

// Folders first, then files, each in `by` order. False with `error` set when
// the folder cannot be read; entries it could not stat are left out.
bool list_dir(const std::string& dir, bool show_hidden, Sort by, bool descending,
              std::vector<Entry>& out, std::string& error);

// By extension alone; a PLY is Splat here and probe_ply() tells it apart.
Kind kind_of(const std::string& name);

struct PlyInfo {
    bool ok = false;
    std::string format;            // "binary_little_endian", "ascii", ...
    int64_t vertices = 0, faces = 0;
    int sh_degree = -1;            // splats only
    Kind kind = Kind::PointCloud;  // Splat, Mesh or PointCloud
    std::vector<std::string> properties;   // the vertex element's
};

// Reads the header only, however large the file.
PlyInfo probe_ply(const std::string& path);

// Up to `max_bytes` of a file. `binary` when it holds a NUL or is not UTF-8;
// `truncated` when there was more.
bool read_text(const std::string& path, size_t max_bytes, std::string& out,
               bool& binary, bool& truncated, std::string& error);

// `dir` + a relative path from somewhere else (an upload's name), or "" when
// it is absolute or climbs out with "..".
std::string join_inside(const std::string& dir, const std::string& relative);

std::string format_size(uint64_t bytes);

}  // namespace storage
}  // namespace gui
