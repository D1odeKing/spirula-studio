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

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

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
    int64_t mtime = 0;
};

int64_t stamp_of(const fs::file_time_type& t) {
    return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch())
        .count();
}

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
        out.push_back({rel, (uint64_t)it->file_size(ec), stamp_of(it->last_write_time(ec))});
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

namespace {

// One big file read by one stream crawls on object storage; several byte
// ranges of it at once do not.
constexpr uint64_t kBigFile = 256ull << 20, kChunk = 64ull << 20;

bool copy_ranges(const fs::path& a, const fs::path& b, uint64_t size, std::string& err) {
#ifdef _WIN32
    std::error_code e;
    fs::copy_file(a, b, fs::copy_options::overwrite_existing, e);
    if (e) err = e.message();
    return !e;
#else
    const int in = ::open(a.c_str(), O_RDONLY);
    const int out = ::open(b.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    bool ok = in >= 0 && out >= 0 && ::ftruncate(out, (off_t)size) == 0;
    std::atomic<uint64_t> next{0};
    std::atomic<bool> failed{!ok};
    auto work = [&] {
        std::vector<char> buf(8u << 20);
        for (uint64_t c; !failed && (c = next++ * kChunk) < size;) {
            for (uint64_t at = c, end = std::min(size, c + kChunk); at < end && !failed;) {
                const ssize_t n = ::pread(in, buf.data(), (size_t)std::min<uint64_t>(buf.size(), end - at), (off_t)at);
                if (n <= 0 || ::pwrite(out, buf.data(), (size_t)n, (off_t)at) != n) failed = true;
                at += n > 0 ? (uint64_t)n : 0;
            }
        }
    };
    if (ok) {
        std::vector<std::thread> pool;
        for (int t = 0; t < 8; t++) pool.emplace_back(work);
        for (std::thread& t : pool) t.join();
    }
    if (in >= 0) ::close(in);
    if (out >= 0 && ::close(out) != 0) failed = true;
    if (failed) err = "read or write failed";
    return !failed;
#endif
}

// Beside the target, then renamed: an interrupted copy never leaves a file
// that passes the size check. The source's time goes with it.
bool copy_one(const fs::path& a, const fs::path& b, uint64_t size, std::string& err) {
    std::error_code e;
    fs::create_directories(b.parent_path(), e);
    const fs::path part = b.u8string() + ".stage-part";
    bool ok;
    if (size >= kBigFile) {
        ok = copy_ranges(a, part, size, err);
    } else {
        fs::copy_file(a, part, fs::copy_options::overwrite_existing, e);
        ok = !e;
        if (e) err = e.message();
    }
    if (ok) fs::rename(part, b, e);
    if (!ok || e) {
        if (ok) err = e.message();
        fs::remove(part, e);
        return false;
    }
    fs::last_write_time(b, fs::last_write_time(a, e), e);
    return true;
}

// `items` from `src` to `dst`, `streams` at a time; with `check`, an item
// already there unchanged is skipped. Indices of the ones copied in `done`.
bool copy_items(const fs::path& src, const fs::path& dst, const std::vector<Item>& items,
                bool check, int streams, const ProgressFn& progress,
                const std::atomic<bool>* cancel, CopyResult& out, std::vector<char>& done,
                std::string& error) {
    Progress pr;
    pr.files_total = items.size();
    for (const Item& i : items) pr.bytes_total += i.size;
    done.assign(items.size(), 0);
    std::mutex mu;
    size_t next = 0;
    std::string first_error;
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
            if (!check || !same(a, b)) {
                copied = copy_one(a, b, it.size, err);
                if (!copied) err = it.rel + ": " + err;
            }
            std::lock_guard<std::mutex> lk(mu);
            if (!err.empty() && first_error.empty()) first_error = err;
            if (copied) done[k] = 1;
            (copied ? out.copied : out.skipped)++;
            if (copied) out.bytes += it.size;
            pr.files_done++;
            pr.bytes_done += it.size;
            if (progress) progress(pr);
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
    return true;
}

}  // namespace

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
    std::vector<char> done;
    if (!copy_items(src, dst, items, true, streams, progress, cancel, out, done, error))
        return false;
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

Snapshot snapshot_of(const std::string& dir, const SkipFn& skip) {
    std::vector<Item> items;
    list(fs::u8path(dir), skip, items);
    Snapshot s;
    for (const Item& i : items) s[i.rel] = {i.size, i.mtime};
    return s;
}

bool sync_changes(const std::string& from, const std::string& to, Snapshot& snap, int streams,
                  const SkipFn& skip, const ProgressFn& progress, CopyResult& out,
                  std::string& error) {
    out = CopyResult{};
    const fs::path src = fs::u8path(from), dst = fs::u8path(to);
    std::vector<Item> now;
    list(src, skip, now);
    std::vector<Item> changed;
    std::set<std::string> present;
    for (const Item& i : now) {
        present.insert(i.rel);
        const auto it = snap.find(i.rel);
        if (it == snap.end() || it->second.size != i.size || it->second.mtime != i.mtime)
            changed.push_back(i);
    }
    std::vector<char> done;
    const bool ok =
        copy_items(src, dst, changed, false, streams, progress, nullptr, out, done, error);
    for (size_t k = 0; k < changed.size(); k++)
        if (done[k]) snap[changed[k].rel] = {changed[k].size, changed[k].mtime};
    if (!ok) return false;
    std::error_code ec;
    for (auto it = snap.begin(); it != snap.end();) {
        if (present.count(it->first)) {
            ++it;
            continue;
        }
        fs::remove(dst / fs::u8path(it->first), ec);
        out.removed++;
        it = snap.erase(it);
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
