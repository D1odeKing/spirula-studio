#pragma once

// A dataset on a network volume (object storage behind FUSE, NFS, SMB) is slow
// per file and fast in parallel: a run reading and writing thousands of frames
// there spends its time waiting. Staging copies the dataset to a local disk in
// parallel streams, runs there, and copies what changed back.
// docs/notes/staging.md.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace gui {
namespace staging {

// The file system `path` lives on, from /proc/self/mounts ("" off Linux or
// when unknown), and whether that kind is a network one.
std::string fs_type_of(const std::string& path);
std::string fs_type_in(const std::string& mounts_text, const std::string& path);
bool is_network_fs(const std::string& fs_type);

// Bytes in `dir`, following links, leaving out what `skip` says.
using SkipFn = std::function<bool(const std::string& relative)>;
uint64_t tree_size(const std::string& dir, const SkipFn& skip);

// Where to stage: `preferred` when set, else /data. "" with `why` when it is
// missing, not a local disk, or short of `need` bytes plus a margin.
std::string pick_fast_root(const std::string& preferred, uint64_t need, std::string& why);

struct Progress {
    uint64_t files_done = 0, files_total = 0;
    uint64_t bytes_done = 0, bytes_total = 0;
};
using ProgressFn = std::function<void(const Progress&)>;

struct CopyResult {
    uint64_t copied = 0, skipped = 0, removed = 0, bytes = 0;
    std::vector<std::string> files;   // relative paths present in `from` after the copy
};

// `from` into `to`, `streams` files at a time, skipping any whose size and time
// match. Deletes from `to` what `mirror_of` lists and `from` lost (a run's own
// deletions), or with `prune` everything `from` lacks. `cancel`: between files.
bool copy_tree(const std::string& from, const std::string& to, int streams, const SkipFn& skip,
               const std::vector<std::string>* mirror_of, bool prune,
               const ProgressFn& progress, const std::atomic<bool>* cancel, CopyResult& out,
               std::string& error);

// What a folder held, file by file, to tell later what changed in it without
// asking the other side about every file.
struct FileStamp {
    uint64_t size = 0;
    int64_t mtime = 0;   // milliseconds
};
using Snapshot = std::map<std::string, FileStamp>;
Snapshot snapshot_of(const std::string& dir, const SkipFn& skip);

// What changed in `from` since `snap` -- new, changed, deleted -- done to `to`,
// and `snap` brought up to date. Only `from` is listed.
bool sync_changes(const std::string& from, const std::string& to, Snapshot& snap, int streams,
                  const SkipFn& skip, const ProgressFn& progress, CopyResult& out,
                  std::string& error);

// In each small text file under `dir` (.json .txt .yaml .yml .cfg .ini .csv),
// `from` becomes `to`: records written in the staged copy name the real place.
int rewrite_paths(const std::string& dir, const std::string& from, const std::string& to);

}  // namespace staging
}  // namespace gui
