// Local copies of what is opened from a network volume (LocalCopies.h).

#include "app/gui/LocalCopies.h"

#include "app/gui/Storage.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace gui {

namespace {

constexpr const char* kIndex = ".spirula-local-index";
constexpr int kStreams = 16;

std::string absolute_of(const std::string& p) {
    std::error_code ec;
    const fs::path a = fs::weakly_canonical(fs::absolute(fs::u8path(p), ec), ec);
    std::string s = a.generic_u8string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

bool within(const std::string& path, const std::string& dir) {
    return path == dir || (path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 &&
                           path[dir.size()] == '/');
}

}  // namespace

LocalCopies::~LocalCopies() { join(); }

void LocalCopies::join() {
    if (_worker.joinable()) _worker.join();
    if (_syncer.joinable()) _syncer.join();
}

void LocalCopies::configure(bool enabled, const std::string& dir) {
    join();
    std::lock_guard<std::mutex> lk(_mu);
    _enabled = false;
    _snap.clear();
    _regions.clear();
    const std::string base = dir.empty() ? "/data" : dir;
    std::error_code ec;
    if (!enabled || !fs::is_directory(fs::u8path(base), ec) ||
        staging::is_network_fs(staging::fs_type_of(base)))
        return;
    _root = absolute_of(base) + "/spirula-local";
    fs::create_directories(fs::u8path(_root), ec);
    if (ec) return;
    _enabled = true;
    load_index();
}

bool LocalCopies::is_local_copy(const std::string& path) const {
    return !_root.empty() && within(absolute_of(path), _root);
}

bool LocalCopies::wants(const std::string& path) const {
    if (!_enabled || path.empty() || is_local_copy(path)) return false;
    std::error_code ec;
    if (!fs::exists(fs::u8path(path), ec)) return false;
    return on_network ? on_network(path) : staging::is_network_fs(staging::fs_type_of(path));
}

bool LocalCopies::wants_any(const std::vector<std::string>& paths) const {
    for (const std::string& p : paths)
        if (wants(p)) return true;
    return false;
}

std::string LocalCopies::local_for(const std::string& path) const {
    if (_root.empty() || is_local_copy(path)) return path;
    return _root + absolute_of(path);
}

std::string LocalCopies::real_for(const std::string& path) const {
    if (_root.empty()) return path;
    const std::string a = absolute_of(path);
    if (!within(a, _root) || a == _root) return path;
    return a.substr(_root.size());
}

bool LocalCopies::has_copies() const {
    std::lock_guard<std::mutex> lk(_mu);
    return !_regions.empty();
}

LocalCopies::Status LocalCopies::status() const {
    std::lock_guard<std::mutex> lk(_mu);
    return _status;
}

std::optional<LocalCopies::Result> LocalCopies::take_result() {
    std::lock_guard<std::mutex> lk(_mu);
    std::optional<Result> r;
    r.swap(_result);
    return r;
}

LocalCopies::SyncReport LocalCopies::last_sync() const {
    std::lock_guard<std::mutex> lk(_mu);
    return _last;
}

// Under the mirror's root, what may go back: inside a region (a copied path's
// folder), or on the way down to one. Everything else is left alone.
staging::SkipFn LocalCopies::sync_skip() const {
    std::vector<std::string> regions;
    {
        std::lock_guard<std::mutex> lk(_mu);
        regions.assign(_regions.begin(), _regions.end());
    }
    return [regions](const std::string& rel) {
        if (rel == kIndex) return true;
        if (rel.size() > 11 && rel.compare(rel.size() - 11, 11, ".stage-part") == 0) return true;
        const std::string abs = "/" + rel;
        for (const std::string& r : regions)
            if (within(abs, r) || within(r, abs)) return false;
        return true;
    };
}

bool LocalCopies::sync_now(std::string& error) {
    staging::Snapshot snap;
    {
        std::lock_guard<std::mutex> lk(_mu);
        snap = _snap;
    }
    staging::CopyResult res;
    const bool ok =
        staging::sync_changes(_root, "/", snap, kStreams, sync_skip(), nullptr, res, error);
    std::lock_guard<std::mutex> lk(_mu);
    _snap = std::move(snap);
    _last.copied = res.copied;
    _last.bytes = res.bytes;
    _last.removed = res.removed;
    _last.error = ok ? "" : error;
    _last.at = (int64_t)std::time(nullptr);
    save_index();
    return ok;
}

void LocalCopies::sync(bool wait) {
    if (!_enabled || _copying || _syncing || !has_copies()) return;
    if (_syncer.joinable()) _syncer.join();
    _syncing = true;
    auto run = [this] {
        std::lock_guard<std::mutex> one(_sync_mu);
        std::string error;
        sync_now(error);
        _syncing = false;
    };
    if (wait) run();
    else _syncer = std::thread(run);
}

void LocalCopies::start(const std::vector<std::string>& paths, bool skip_outputs) {
    if (_worker.joinable()) _worker.join();
    if (_syncer.joinable()) _syncer.join();
    _cancel = false;
    _copying = true;
    {
        std::lock_guard<std::mutex> lk(_mu);
        _status = Status{};
        _status.items = (int)paths.size();
        _result.reset();
    }
    _worker = std::thread([this, paths, skip_outputs] {
        std::lock_guard<std::mutex> one(_sync_mu);
        const auto t0 = std::chrono::steady_clock::now();
        Result r;
        r.ok = true;
        // What changed here goes back first: the copy below makes the mirror
        // the volume's, and must not overwrite work that never left.
        std::string error;
        const bool synced = !has_copies() || sync_now(error);
        for (size_t k = 0; k < paths.size() && r.ok; k++) {
            const std::string& p = paths[k];
            if (!wants(p)) {
                r.paths.push_back(p);
                continue;
            }
            const std::string real = absolute_of(p);
            const std::string local = _root + real;
            std::error_code ec;
            const bool dir = fs::is_directory(fs::u8path(real), ec);
            // A file is copied as the one entry of its folder that is wanted.
            const std::string from = dir ? real : fs::u8path(real).parent_path().generic_u8string();
            const std::string to = dir ? local : fs::u8path(local).parent_path().generic_u8string();
            const std::string name = fs::u8path(real).filename().u8string();
            staging::SkipFn skip = [dir, name, skip_outputs](const std::string& rel) {
                if (rel.size() > 11 && rel.compare(rel.size() - 11, 11, ".stage-part") == 0)
                    return true;
                if (!dir) return rel != name;
                return skip_outputs && (rel == "outputs" || rel.rfind("outputs/", 0) == 0);
            };
            {
                std::lock_guard<std::mutex> lk(_mu);
                _status.name = name;
                _status.item = (int)k + 1;
                _regions.insert(dir ? real : fs::u8path(real).parent_path().generic_u8string());
                if (dir) _regions.insert(fs::u8path(real).parent_path().generic_u8string());
            }
            const fs::space_info sp = fs::space(fs::u8path(_root), ec);
            bool roomy_checked = false;
            auto progress = [&](const staging::Progress& pr) {
                if (!roomy_checked) {
                    roomy_checked = true;
                    if (!ec && pr.bytes_total + pr.bytes_total / 10 > sp.available) {
                        error = "not enough room on the local disk: " +
                                storage::format_size(pr.bytes_total) + " to copy, " +
                                storage::format_size(sp.available) + " free";
                        _cancel = true;
                    }
                }
                std::lock_guard<std::mutex> lk(_mu);
                _status.progress = pr;
                _status.seconds = std::chrono::duration<double>(
                                      std::chrono::steady_clock::now() - t0).count();
            };
            staging::CopyResult res;
            std::string err;
            if (!staging::copy_tree(from, to, kStreams, skip, nullptr, /*prune=*/synced,
                                    progress, &_cancel, res, err)) {
                r.ok = false;
                r.error = !error.empty() ? error : _cancel ? "cancelled" : err;
                break;
            }
            r.copied += res.copied;
            r.bytes += res.bytes;
            r.skipped += res.skipped;
            // What the volume now has is what the mirror has: not a change.
            const staging::Snapshot here = staging::snapshot_of(to, skip);
            const std::string prefix = to.substr(_root.size() + 1) + "/";
            std::lock_guard<std::mutex> lk(_mu);
            for (auto it = _snap.lower_bound(prefix);
                 it != _snap.end() && it->first.compare(0, prefix.size(), prefix) == 0;) {
                const std::string rel = it->first.substr(prefix.size());
                if (!skip(rel) && !here.count(rel)) it = _snap.erase(it);
                else ++it;
            }
            for (const auto& [rel, st] : here) _snap[prefix + rel] = st;
            r.paths.push_back(local);
        }
        std::lock_guard<std::mutex> lk(_mu);
        save_index();
        _result = std::move(r);
        _copying = false;
    });
}

void LocalCopies::load_index() {
    std::ifstream f(fs::u8path(_root) / kIndex);
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("region ", 0) == 0) {
            _regions.insert(line.substr(7));
            continue;
        }
        unsigned long long size = 0;
        long long mtime = 0;
        int used = 0;
        if (std::sscanf(line.c_str(), "file %llu %lld %n", &size, &mtime, &used) >= 2 && used > 0)
            _snap[line.substr((size_t)used)] = {size, mtime};
    }
}

void LocalCopies::save_index() const {
    if (_root.empty()) return;
    const fs::path file = fs::u8path(_root) / kIndex;
    const fs::path tmp = file.u8string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        for (const std::string& r : _regions) f << "region " << r << "\n";
        for (const auto& [rel, st] : _snap)
            f << "file " << st.size << " " << st.mtime << " " << rel << "\n";
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
}

}  // namespace gui
