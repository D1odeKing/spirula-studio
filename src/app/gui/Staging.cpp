// Staging a dataset on the local disk (Staging.h).

#include "app/gui/Staging.h"

#include "app/gui/Storage.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace gui {
namespace staging {

namespace {

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

bool under(const std::string& path, const std::string& mount) {
    if (mount == "/") return true;
    return path.compare(0, mount.size(), mount) == 0 &&
           (path.size() == mount.size() || path[mount.size()] == '/');
}

// The nearest existing ancestor, resolved: a workspace about to be created
// lives where its parent does.
std::string resolved(const std::string& path) {
    std::error_code ec;
    fs::path p = fs::absolute(fs::u8path(path), ec);
    while (!p.empty() && !fs::exists(p, ec) && p != p.parent_path()) p = p.parent_path();
    const fs::path c = fs::canonical(p, ec);
    return (ec ? p : c).u8string();
}

struct Item {
    std::string rel;
    uint64_t size = 0;
};

void list(const fs::path& root, const SkipFn& skip, std::vector<Item>& out) {
    std::error_code ec;
    const auto opts = fs::directory_options::follow_directory_symlink |
                      fs::directory_options::skip_permission_denied;
    for (fs::recursive_directory_iterator it(root, opts, ec), end; !ec && it != end;
         it.increment(ec)) {
        const std::string rel = fs::relative(it->path(), root, ec).generic_u8string();
        if (ec) break;
        if (skip && skip(rel)) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        out.push_back({rel, (uint64_t)it->file_size(ec)});
    }
}

bool same(const fs::path& a, const fs::path& b) {
    std::error_code ec, ec2;
    if (!fs::exists(b, ec)) return false;
    if (fs::file_size(a, ec) != fs::file_size(b, ec2) || ec || ec2) return false;
    const auto ta = fs::last_write_time(a, ec), tb = fs::last_write_time(b, ec2);
    if (ec || ec2) return true;   // a volume without times: size is all there is
    // Copies keep their source's time, so a second covers a volume that stores
    // whole seconds; a file written since is newer than that.
    return ta <= tb + std::chrono::seconds(1);
}

}  // namespace

std::string fs_type_in(const std::string& mounts_text, const std::string& path) {
    std::string best_mount, best_type;
    std::istringstream in(mounts_text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream f(line);
        std::string dev, mnt, type;
        if (!(f >> dev >> mnt >> type)) continue;
        // /proc/self/mounts escapes a space in a path as \040.
        for (size_t i; (i = mnt.find("\\040")) != std::string::npos;) mnt.replace(i, 4, " ");
        if (under(path, mnt) && mnt.size() >= best_mount.size()) {
            best_mount = mnt;
            best_type = type;
        }
    }
    return best_type;
}

std::string fs_type_of(const std::string& path) {
#ifdef __linux__
    return fs_type_in(read_file("/proc/self/mounts"), resolved(path));
#else
    (void)path;
    return "";
#endif
}

bool is_network_fs(const std::string& t) {
    if (t.rfind("fuse.", 0) == 0) return t != "fuse.lxcfs";
    static const char* kNet[] = {"fuse", "nfs", "nfs4", "cifs", "smb3", "smbfs", "9p",
                                 "ceph", "glusterfs", "lustre", "gpfs", "afs", "virtiofs"};
    for (const char* k : kNet)
        if (t == k) return true;
    return false;
}

uint64_t tree_size(const std::string& dir, const SkipFn& skip) {
    std::vector<Item> items;
    list(fs::u8path(dir), skip, items);
    uint64_t n = 0;
    for (const Item& i : items) n += i.size;
    return n;
}

std::string pick_fast_root(const std::string& preferred, uint64_t need, std::string& why) {
    const std::string root = preferred.empty() ? "/data" : preferred;
    std::error_code ec;
    if (!fs::is_directory(fs::u8path(root), ec)) {
        why = root + " does not exist";
        return "";
    }
    const std::string type = fs_type_of(root);
    if (is_network_fs(type)) {
        why = root + " is itself a network volume (" + type + ")";
        return "";
    }
    const fs::space_info sp = fs::space(fs::u8path(root), ec);
    // A run writes its own files beside the copy: frames, features, a model.
    const uint64_t want = need + need / 2 + (2ull << 30);
    if (!ec && sp.available < want) {
        why = root + " has " + storage::format_size(sp.available) + " free, " +
              storage::format_size(want) + " wanted";
        return "";
    }
    return root;
}

bool copy_tree(const std::string& from, const std::string& to, int streams, const SkipFn& skip,
               const std::vector<std::string>* mirror_of, bool prune,
               const ProgressFn& progress, const std::atomic<bool>* cancel, CopyResult& out,
               std::string& error) {
    out = CopyResult{};
    const fs::path src = fs::u8path(from), dst = fs::u8path(to);
    std::error_code ec;
    fs::create_directories(dst, ec);
    if (ec) {
        error = "cannot create " + to + ": " + ec.message();
        return false;
    }
    std::vector<Item> items;
    list(src, skip, items);
    Progress pr;
    pr.files_total = items.size();
    for (const Item& i : items) pr.bytes_total += i.size;

    std::mutex mu;
    size_t next = 0;
    std::string first_error;
    auto report = [&] { if (progress) progress(pr); };
    auto worker = [&] {
        for (;;) {
            size_t k;
            {
                std::lock_guard<std::mutex> lk(mu);
                if (next >= items.size() || !first_error.empty()) return;
                if (cancel && cancel->load()) return;
                k = next++;
            }
            const Item& it = items[k];
            const fs::path a = src / fs::u8path(it.rel), b = dst / fs::u8path(it.rel);
            bool copied = false;
            std::string err;
            if (!same(a, b)) {
                std::error_code e;
                fs::create_directories(b.parent_path(), e);
                // Beside the target, then renamed: an interrupted copy never
                // leaves a file that passes the size check.
                const fs::path part = b.u8string() + ".stage-part";
                fs::copy_file(a, part, fs::copy_options::overwrite_existing, e);
                if (!e) fs::rename(part, b, e);
                if (e) {
                    fs::remove(part, e);
                    err = it.rel + ": " + e.message();
                } else {
                    fs::last_write_time(b, fs::last_write_time(a, e), e);
                    copied = true;
                }
            }
            std::lock_guard<std::mutex> lk(mu);
            if (!err.empty() && first_error.empty()) first_error = err;
            (copied ? out.copied : out.skipped)++;
            if (copied) out.bytes += it.size;
            pr.files_done++;
            pr.bytes_done += it.size;
            report();
        }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < std::max(1, streams); t++) pool.emplace_back(worker);
    for (std::thread& t : pool) t.join();
    if (!first_error.empty()) {
        error = first_error;
        return false;
    }
    if (cancel && cancel->load()) {
        error = "cancelled";
        return false;
    }
    for (const Item& i : items) out.files.push_back(i.rel);
    const std::set<std::string> now(out.files.begin(), out.files.end());
    if (mirror_of)
        for (const std::string& rel : *mirror_of) {
            if (now.count(rel) || (skip && skip(rel))) continue;
            if (fs::remove(dst / fs::u8path(rel), ec)) out.removed++;
        }
    if (prune) {
        std::vector<Item> extra;
        list(dst, skip, extra);
        for (const Item& i : extra)
            if (!now.count(i.rel) && fs::remove(dst / fs::u8path(i.rel), ec)) out.removed++;
    }
    return true;
}

int rewrite_paths(const std::string& dir, const std::string& from, const std::string& to) {
    if (from.empty() || from == to) return 0;
    static const char* kExt[] = {".json", ".txt", ".yaml", ".yml", ".cfg", ".ini", ".csv"};
    int n = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(fs::u8path(dir), ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->file_size(ec) > (8u << 20)) continue;
        std::string ext = it->path().extension().u8string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (std::find_if(std::begin(kExt), std::end(kExt),
                         [&](const char* e) { return ext == e; }) == std::end(kExt))
            continue;
        std::string s = read_file(it->path());
        bool changed = false;
        for (size_t i = 0; (i = s.find(from, i)) != std::string::npos; i += to.size()) {
            s.replace(i, from.size(), to);
            changed = true;
        }
        if (!changed) continue;
        const auto t = fs::last_write_time(it->path(), ec);
        std::ofstream(it->path(), std::ios::binary | std::ios::trunc) << s;
        fs::last_write_time(it->path(), t, ec);
        n++;
    }
    return n;
}

}  // namespace staging
}  // namespace gui
