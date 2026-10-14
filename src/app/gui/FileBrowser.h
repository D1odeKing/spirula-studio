#pragma once

// The Files window: every mounted storage, a folder's files, and a look inside
// the selected one -- a picture, a video's frames, a text file, a PLY's header,
// a cloud's points -- plus moving files to and from another computer. A window
// of its own over whatever screen is up, so a run keeps going behind it.
// docs/notes/file-browser.md.

#include "app/gui/FileDialog.h"
#include "app/gui/Picture.h"
#include "app/gui/PreviewTexture.h"
#include "app/gui/SourcePreview.h"
#include "app/gui/Storage.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gui {

class FileBrowser {
public:
    // What the app lends it, asked every frame.
    struct Host {
        std::function<void(const std::string&)> open_here;   // as a drop would
        std::function<bool()> gpu_allowed;   // may a video decode on the device now
        std::string ffmpeg = "ffmpeg";
        bool builtin_video = false;
    };

    ~FileBrowser();

    void open(const std::string& dir = "");
    bool is_open() const { return _open; }
    void draw(const Host& host);
    // A video playing wants every frame drawn.
    bool animating() const { return _open && _preview.animating(); }
    void destroy_gl();

private:
    struct Listing;
    struct Look;

    void go(const std::string& dir, bool remember = true);
    void start_listing();
    void poll_listing();
    void select(const storage::Entry& e);
    void start_look();
    void poll_look();
    void draw_locations();
    void draw_toolbar();
    void draw_table(float height);
    void draw_preview(const Host& host);
    void draw_actions(const Host& host);
    void draw_new_folder();
    void start_copy(std::vector<std::string> from, const std::string& to_dir);
    void poll_copy();
    std::string selected_path() const;

    bool _open = false;
    std::string _cwd;
    std::vector<std::string> _back;
    std::string _path_edit;
    std::string _filter;
    bool _show_hidden = false;
    storage::Sort _sort = storage::Sort::Name;
    bool _descending = false;

    std::vector<storage::Location> _locations;
    double _locations_at = -1e9;

    // The folder's entries come off a worker: a volume on object storage
    // takes seconds to stat ten thousand files.
    std::thread _list_thread;
    std::mutex _mu;
    std::shared_ptr<Listing> _listing_ready;   // published by the worker
    std::shared_ptr<Listing> _listing;         // drawn
    bool _listing_busy = false;
    uint64_t _uploads_seen = 0;

    std::string _sel;                          // a name in _cwd
    storage::Entry _sel_entry;

    // The selected file's own look: pixels, text or a PLY header.
    std::thread _look_thread;
    std::shared_ptr<Look> _look_ready;
    std::shared_ptr<Look> _look;
    PreviewTexture _tex;
    std::string _tex_path;

    // Videos, photo folders and point clouds, as the dataset screen shows them.
    SourcePreview _preview;
    bool _show_cloud = false;
    bool _show_photos = false;

    FileDialog _dialog;
    enum class Pick { None, Export, ImportFiles, ImportFolder };
    Pick _pick = Pick::None;

    std::thread _copy_thread;
    std::atomic<bool> _copy_busy{false};
    bool _copy_was_busy = false;
    std::string _copy_status;                  // guarded by _mu

    bool _want_new_folder = false;
    std::string _new_folder;
    std::string _new_folder_error;
};

}  // namespace gui
