#pragma once

// What is opened from a network volume is opened from a local copy of it.
// The copy is a mirror under one root: /mnt/vol/a/b lives at <root>/mnt/vol/a/b,
// so anything written beside it locally -- a dataset made next to its photos,
// a run's outputs -- has a place on the volume too, and goes back there.
// docs/notes/staging.md.

#include "app/gui/Staging.h"

#include <atomic>
#include <functional>
#include <set>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace gui {

class LocalCopies {
public:
    ~LocalCopies();

    // `dir` is the local disk ("" is /data); the mirror is <dir>/spirula-local.
    // Reads what an earlier session left there.
    void configure(bool enabled, const std::string& dir);
    bool enabled() const { return _enabled; }
    const std::string& root() const { return _root; }

    // Whether `path` would be copied before it is opened: on a network volume,
    // not already in the mirror, and copying is on.
    bool wants(const std::string& path) const;
    bool wants_any(const std::vector<std::string>& paths) const;
    // The mirror's copy of a volume path, and back.
    std::string local_for(const std::string& path) const;
    std::string real_for(const std::string& path) const;
    bool is_local_copy(const std::string& path) const;

    // Copies `paths` (files or folders) in the background, first sending back
    // what changed since the last sync. `skip_outputs` leaves a folder's
    // top-level outputs/ on the volume.
    void start(const std::vector<std::string>& paths, bool skip_outputs);
    bool copying() const { return _copying; }
    void cancel() { _cancel = true; }
    struct Status {
        std::string name;               // what is being copied now
        int item = 0, items = 0;
        staging::Progress progress;
        double seconds = 0;
    };
    Status status() const;
    // Once, when a copy started with start() ended: the mapped paths, or the
    // error ("cancelled" when cancelled).
    struct Result {
        bool ok = false;
        std::vector<std::string> paths;
        std::string error;
        uint64_t copied = 0, bytes = 0, skipped = 0;
    };
    std::optional<Result> take_result();

    // Sends back what changed in the mirror. In the background unless `wait`.
    void sync(bool wait = false);
    bool syncing() const { return _syncing; }
    bool has_copies() const;
    struct SyncReport {
        uint64_t copied = 0, bytes = 0, removed = 0;
        std::string error;
        int64_t at = 0;   // seconds since the epoch, 0 never
    };
    SyncReport last_sync() const;

    // Whether a path is on a network volume; the file system's kind unless a
    // test says otherwise.
    std::function<bool(const std::string&)> on_network;

private:
    void load_index();
    void save_index() const;    // under _mu
    bool sync_now(std::string& error);
    staging::SkipFn sync_skip() const;
    void join();

    bool _enabled = false;
    std::string _root;
    mutable std::mutex _mu;
    staging::Snapshot _snap;    // the mirror as the volume last saw it
    std::set<std::string> _regions;   // volume folders the mirror may write back to
    std::thread _worker, _syncer;
    std::atomic<bool> _copying{false}, _syncing{false}, _cancel{false};
    Status _status;
    std::optional<Result> _result;
    SyncReport _last;
    std::mutex _sync_mu;        // one sync at a time, and none during a copy
};

}  // namespace gui
