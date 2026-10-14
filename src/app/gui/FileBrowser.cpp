// FileBrowser.cpp -- see FileBrowser.h.

#include "app/gui/FileBrowser.h"

#include "app/AppPaths.h"
#include "app/gui/Layout.h"
#include "app/gui/RemoteDisplay.h"
#include "app/gui/Subprocess.h"
#include "app/gui/Ui.h"
#include "i18n/catalog/Files.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace gui {

namespace fmsg = spirula::i18n::msg::files;
using storage::Kind;

struct FileBrowser::Listing {
    std::string dir;
    std::vector<storage::Entry> entries;
    std::string error;
};

struct FileBrowser::Look {
    std::string path;
    Kind kind = Kind::Other;
    Picture picture;
    bool picture_ok = false;
    std::string text;
    bool binary = false, truncated = false;
    storage::PlyInfo ply;
};

namespace {

constexpr size_t kTextBytes = 1 << 20;
constexpr int kPictureSide = 2048;

const spirula::i18n::Msg& kind_label(Kind k) {
    switch (k) {
        case Kind::Folder: return fmsg::kind_folder;
        case Kind::Image: return fmsg::kind_image;
        case Kind::Video: return fmsg::kind_video;
        case Kind::Text: return fmsg::kind_text;
        case Kind::Splat: return fmsg::kind_splat;
        case Kind::PointCloud: return fmsg::kind_cloud;
        case Kind::Mesh: return fmsg::kind_mesh;
        case Kind::Archive: return fmsg::kind_archive;
        default: return fmsg::kind_other;
    }
}

std::string format_time(int64_t t) {
    if (t <= 0) return {};
    const std::time_t tt = (std::time_t)t;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// A splat, cloud or mesh the web viewer reads.
bool viewer_reads(const std::string& name, Kind k) {
    const std::string n = lower(name);
    auto ends = [&](const char* e) {
        const size_t len = std::strlen(e);
        return n.size() >= len && n.compare(n.size() - len, len, e) == 0;
    };
    return k == Kind::Splat ? ends(".ply") : k == Kind::Mesh;
}

uint64_t tree_bytes(const fs::path& p) {
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) return fs::file_size(p, ec);
    uint64_t total = 0;
    for (fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec))
        if (!ec && it->is_regular_file(ec)) total += it->file_size(ec);
    return total;
}

}  // namespace

FileBrowser::~FileBrowser() {
    if (_list_thread.joinable()) _list_thread.join();
    if (_look_thread.joinable()) _look_thread.join();
    if (_copy_thread.joinable()) _copy_thread.join();
}

void FileBrowser::open(const std::string& dir) {
    _open = true;
    std::error_code ec;
    if (!dir.empty() && fs::is_directory(fs::u8path(dir), ec)) go(dir, false);
    else if (_cwd.empty()) go(fs::current_path(ec).u8string(), false);
    _locations_at = -1e9;
}

void FileBrowser::destroy_gl() {
    _tex.reset();
    _tex_path.clear();
    _preview.destroy_gl();
    _preview.clear();
}

std::string FileBrowser::selected_path() const {
    if (_sel.empty()) return {};
    return (fs::u8path(_cwd) / fs::u8path(_sel)).u8string();
}

void FileBrowser::go(const std::string& dir, bool remember) {
    if (remember && !_cwd.empty() && dir != _cwd) _back.push_back(_cwd);
    if (_back.size() > 64) _back.erase(_back.begin());
    _cwd = dir;
    _path_edit = dir;
    _sel.clear();
    _show_cloud = _show_photos = false;
    _preview.clear();
    start_listing();
}

void FileBrowser::start_listing() {
    if (_list_thread.joinable()) _list_thread.join();
    _listing_busy = true;
    const std::string dir = _cwd;
    const bool hidden = _show_hidden;
    const storage::Sort by = _sort;
    const bool desc = _descending;
    _list_thread = std::thread([this, dir, hidden, by, desc] {
        auto l = std::make_shared<Listing>();
        l->dir = dir;
        storage::list_dir(dir, hidden, by, desc, l->entries, l->error);
        std::lock_guard<std::mutex> lk(_mu);
        _listing_ready = l;
    });
}

void FileBrowser::poll_listing() {
    std::shared_ptr<Listing> l;
    {
        std::lock_guard<std::mutex> lk(_mu);
        l.swap(_listing_ready);
    }
    if (!l) return;
    _listing_busy = false;
    if (l->dir == _cwd) _listing = l;
}

void FileBrowser::select(const storage::Entry& e) {
    if (_sel == e.name) return;
    _sel = e.name;
    _sel_entry = e;
    _show_cloud = _show_photos = false;
    _preview.clear();
    start_look();
}

void FileBrowser::start_look() {
    if (_look_thread.joinable()) _look_thread.join();
    auto look = std::make_shared<Look>();
    look->path = selected_path();
    look->kind = _sel_entry.kind;
    _look = nullptr;
    if (look->kind != Kind::Image && look->kind != Kind::Text && look->kind != Kind::Splat &&
        look->kind != Kind::Other)
        return;
    _look_thread = std::thread([this, look] {
        std::string error;
        if (look->kind == Kind::Image) {
            look->picture_ok = load_picture(look->path, "", kPictureSide, look->picture);
        } else if (look->kind == Kind::Splat) {
            look->ply = storage::probe_ply(look->path);
        } else {
            storage::read_text(look->path, kTextBytes, look->text, look->binary,
                               look->truncated, error);
        }
        std::lock_guard<std::mutex> lk(_mu);
        _look_ready = look;
    });
}

void FileBrowser::poll_look() {
    std::shared_ptr<Look> l;
    {
        std::lock_guard<std::mutex> lk(_mu);
        l.swap(_look_ready);
    }
    if (l && l->path == selected_path()) _look = l;
}

void FileBrowser::start_copy(std::vector<std::string> from, const std::string& to_dir) {
    if (_copy_busy.load() || from.empty()) return;
    if (_copy_thread.joinable()) _copy_thread.join();
    _copy_busy = true;
    {
        std::lock_guard<std::mutex> lk(_mu);
        _copy_status = spirula::i18n::format(fmsg::copying,
                                             {fs::u8path(from[0]).filename().u8string()});
    }
    _copy_thread = std::thread([this, from, to_dir] {
        std::error_code ec;
        uint64_t bytes = 0;
        for (const std::string& src : from) {
            const fs::path s = fs::u8path(src);
            const fs::path d = fs::u8path(to_dir) / s.filename();
            bytes += tree_bytes(s);
            fs::copy(s, d, fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
            if (ec) break;
        }
        std::lock_guard<std::mutex> lk(_mu);
        _copy_status =
            ec ? spirula::i18n::format(fmsg::copy_failed, {ec.message()})
               : spirula::i18n::format(fmsg::copy_done,
                                       {from.size() == 1 ? fs::u8path(from[0]).filename().u8string()
                                                         : std::to_string(from.size()),
                                        storage::format_size(bytes)});
        _copy_busy = false;
    });
}

void FileBrowser::poll_copy() {
    const bool busy = _copy_busy.load();
    if (_copy_was_busy && !busy) start_listing();
    _copy_was_busy = busy;
}

// ---------------------------------------------------------------------------

void FileBrowser::draw(const Host& host) {
    if (!_open) {
        remote::set_drop_dir("");
        return;
    }
    poll_listing();
    poll_look();
    poll_copy();
    if (remote::active()) {
        remote::set_drop_dir(_cwd);
        const uint64_t up = remote::upload_count();
        if (up != _uploads_seen) {
            _uploads_seen = up;
            start_listing();
        }
    }
    const double now = ImGui::GetTime();
    if (now - _locations_at > 10.0) {
        _locations = storage::locations();
        _locations_at = now;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.9f, vp->WorkSize.y * 0.86f),
                             ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (!ImGui::Begin(ui::detail::label(fmsg::window_title), &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        _preview.pause();
        if (!open) _open = false;
        return;
    }
    _preview.set_gpu(host.gpu_allowed && host.gpu_allowed(), command_exists(host.ffmpeg));

    draw_toolbar();
    const float h = ImGui::GetContentRegionAvail().y;
    const float side_w = std::clamp(px(230.0f), px(160.0f), ImGui::GetContentRegionAvail().x * 0.2f);
    ImGui::BeginChild("##fileslocations", ImVec2(side_w, h), ImGuiChildFlags_Borders);
    draw_locations();
    ImGui::EndChild();
    ImGui::SameLine();
    const float table_w = std::max(px(320.0f), (ImGui::GetContentRegionAvail().x) * 0.48f);
    ImGui::BeginChild("##filestable", ImVec2(table_w, h), ImGuiChildFlags_None);
    draw_table(h);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##filespreview", ImVec2(0, h), ImGuiChildFlags_Borders);
    draw_preview(host);
    ImGui::EndChild();

    draw_new_folder();
    if (_dialog.draw()) {
        if (_pick == Pick::Export && !_dialog.result().empty() && !_sel.empty())
            start_copy({selected_path()}, _dialog.result());
        else if ((_pick == Pick::ImportFiles || _pick == Pick::ImportFolder) &&
                 !_dialog.results().empty())
            start_copy(_dialog.results(), _cwd);
        _pick = Pick::None;
    }
    ImGui::End();
    if (!open) {
        _open = false;
        _preview.clear();
    }
}

void FileBrowser::draw_toolbar() {
    if (ui::Button(fmsg::back) && !_back.empty()) {
        const std::string to = _back.back();
        _back.pop_back();
        go(to, false);
    }
    ImGui::SameLine();
    if (ui::Button(fmsg::up)) {
        const fs::path p = fs::u8path(_cwd);
        if (p.has_parent_path() && p.parent_path() != p) go(p.parent_path().u8string());
    }
    ImGui::SameLine();
    if (ui::Button(fmsg::refresh)) {
        start_listing();
        _locations_at = -1e9;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - px(330.0f));
    if (ui::InputTextRaw("##filespath", &_path_edit, ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::error_code ec;
        const fs::path p = fs::u8path(_path_edit);
        if (fs::is_directory(p, ec)) go(p.u8string());
        else if (fs::is_regular_file(p, ec)) {
            go(p.parent_path().u8string());
            _sel = p.filename().u8string();
        } else {
            _path_edit = _cwd;
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(180.0f));
    ui::InputTextWithHintRaw("##filesfilter", fmsg::filter_hint, &_filter);
    ImGui::SameLine();
    if (ui::Checkbox(fmsg::show_hidden, &_show_hidden)) start_listing();
}

void FileBrowser::draw_locations() {
    ui::SeparatorText(fmsg::locations);
    for (size_t i = 0; i < _locations.size(); i++) {
        const storage::Location& l = _locations[i];
        ImGui::PushID((int)i);
        const bool here = _cwd == l.path ||
                          (_cwd.rfind(l.path, 0) == 0 && l.path.size() > 1 &&
                           (_cwd.size() == l.path.size() || _cwd[l.path.size()] == '/'));
        if (ui::SelectableRaw(l.label.c_str(), here)) go(l.path);
        std::string tip = l.path;
        if (!l.fs_type.empty()) tip += "  (" + l.fs_type + ")";
        if (l.total)
            tip += "\n" + spirula::i18n::format(fmsg::location_free,
                                                {storage::format_size(l.free),
                                                 storage::format_size(l.total)});
        ui::help_on_hover_raw(tip.c_str());
        if (l.total) {
            const float used = 1.0f - (float)((double)l.free / (double)l.total);
            ui::ProgressBarRaw(used, ImVec2(-1, px(3.0f)), "");
        }
        ImGui::PopID();
    }
}

void FileBrowser::draw_table(float height) {
    if (_listing_busy && (!_listing || _listing->dir != _cwd)) {
        ui::TextDisabled(fmsg::reading);
        return;
    }
    if (!_listing) return;
    if (!_listing->error.empty()) {
        ui::TextWrapped(fmsg::cannot_read, {_listing->error});
        return;
    }
    const std::string filter = lower(_filter);
    ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable |
                            ImGuiTableFlags_BordersInnerV;
    if (_listing->entries.empty()) ui::TextDisabled(fmsg::empty_folder);
    if (!ImGui::BeginTable("##files", 3, flags, ImVec2(0, height - px(26.0f)))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ui::TableSetupColumn(fmsg::col_name, ImGuiTableColumnFlags_WidthStretch |
                                             ImGuiTableColumnFlags_DefaultSort);
    ui::TableSetupColumn(fmsg::col_size, ImGuiTableColumnFlags_WidthFixed, px(80.0f));
    ui::TableSetupColumn(fmsg::col_modified, ImGuiTableColumnFlags_WidthFixed, px(128.0f));
    ImGui::TableHeadersRow();
    if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs(); sort && sort->SpecsDirty) {
        if (sort->SpecsCount > 0) {
            const int col = sort->Specs[0].ColumnIndex;
            _sort = col == 1 ? storage::Sort::Size
                    : col == 2 ? storage::Sort::Modified
                               : storage::Sort::Name;
            _descending = sort->Specs[0].SortDirection == ImGuiSortDirection_Descending;
            start_listing();
        }
        sort->SpecsDirty = false;
    }
    int shown = 0;
    for (size_t i = 0; i < _listing->entries.size(); i++) {
        const storage::Entry& e = _listing->entries[i];
        if (!filter.empty() && lower(e.name).find(filter) == std::string::npos) continue;
        shown++;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID((int)i);
        const std::string label = (e.is_dir ? "[+] " : "    ") + e.name;
        if (ui::SelectableRaw(label.c_str(), _sel == e.name,
                              ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
            if (e.is_dir && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                go((fs::u8path(_cwd) / fs::u8path(e.name)).u8string());
            else
                select(e);
        }
        ImGui::PopID();
        ImGui::TableNextColumn();
        if (!e.is_dir) ui::TextDisabledRaw(storage::format_size(e.size));
        ImGui::TableNextColumn();
        ui::TextDisabledRaw(format_time(e.mtime));
    }
    ImGui::EndTable();
    ui::TextDisabled(fmsg::item_count, {shown});
}

void FileBrowser::draw_preview(const Host& host) {
    if (_sel.empty()) {
        ui::TextDisabledWrapped(fmsg::nothing_selected);
        _preview.pause();
        draw_actions(host);
        return;
    }
    const storage::Entry& e = _sel_entry;
    ui::TextWrappedRaw(e.name);
    ui::TextDisabled(fmsg::size_modified,
                     {std::string(kind_label(e.kind).get()) +
                          (e.is_dir ? "" : ", " + storage::format_size(e.size)),
                      format_time(e.mtime)});
    draw_actions(host);
    ImGui::Separator();

    const std::string path = selected_path();
    const float avail_h = ImGui::GetContentRegionAvail().y;
    PreviewInput in;
    if (e.kind == Kind::Video) {
        in.kind = PreviewInput::Kind::Video;
        in.src.input = path;
        in.src.is_video = true;
        in.src.ffmpeg_exe = host.ffmpeg;
        in.src.builtin_decode = host.builtin_video;
    } else if ((e.kind == Kind::PointCloud || e.kind == Kind::Splat) && _show_cloud) {
        in.kind = PreviewInput::Kind::Scan;
    } else if (e.is_dir && _show_photos) {
        in.kind = PreviewInput::Kind::Photos;
        in.src.input = path;
    }
    if (in.kind != PreviewInput::Kind::None) {
        in.path = path;
        _preview.show(in);
        _preview.draw(avail_h, nullptr, nullptr);
        return;
    }
    _preview.pause();

    if (e.kind == Kind::Image) {
        if (!_look) {
            ui::TextDisabled(fmsg::reading);
        } else if (!_look->picture_ok) {
            ui::TextDisabled(fmsg::image_failed);
        } else {
            if (_tex_path != _look->path) {
                _tex.upload(_look->picture);
                _tex_path = _look->path;
            }
            ImVec2 mn, mx;
            const ImVec2 at = ImGui::GetCursorScreenPos();
            fit_rect((float)_tex.w, (float)_tex.h, at, ImGui::GetContentRegionAvail(), mn, mx);
            ImGui::SetCursorScreenPos(mn);
            ImGui::Image(_tex.id(), ImVec2(mx.x - mn.x, mx.y - mn.y));
        }
        return;
    }
    if (e.kind == Kind::Splat) {
        if (!_look) {
            ui::TextDisabled(fmsg::reading);
            return;
        }
        const storage::PlyInfo& p = _look->ply;
        if (!p.ok) {
            ui::TextDisabled(fmsg::ply_unreadable);
            return;
        }
        if (p.kind == Kind::Splat)
            ui::Text(fmsg::ply_splat, {(long long)p.vertices, p.sh_degree});
        else if (p.kind == Kind::Mesh)
            ui::Text(fmsg::ply_mesh, {(long long)p.vertices, (long long)p.faces});
        else
            ui::Text(fmsg::ply_points, {(long long)p.vertices});
        ui::TextDisabledRaw(p.format);
        if (ui::TreeNode(fmsg::ply_properties)) {
            std::string all;
            for (const std::string& name : p.properties) all += (all.empty() ? "" : ", ") + name;
            ui::TextWrappedRaw(all);
            ImGui::TreePop();
        }
        return;
    }
    if (e.kind == Kind::Text || e.kind == Kind::Other) {
        if (!_look) {
            if (e.kind == Kind::Text) ui::TextDisabled(fmsg::reading);
            return;
        }
        if (_look->binary) {
            if (e.kind == Kind::Text) ui::TextDisabled(fmsg::text_binary);
            return;
        }
        if (_look->truncated)
            ui::TextDisabled(fmsg::text_truncated, {storage::format_size(kTextBytes)});
        ui::InputTextMultilineRaw("##filestext", &_look->text, ImGui::GetContentRegionAvail(),
                                  ImGuiInputTextFlags_ReadOnly);
    }
}

void FileBrowser::draw_actions(const Host& host) {
    const bool remote_on = remote::active();
    const std::string path = selected_path();
    const storage::Entry& e = _sel_entry;
    const bool have = !_sel.empty();

    if (have && viewer_reads(e.name, e.kind)) {
        if (ui::Button(fmsg::open_in_viewer)) {
            if (remote_on) remote::open_in_viewer(path);
            else spawn_detached({app::exe_path(), path});
        }
        ui::help_on_hover(remote_on ? fmsg::open_in_viewer_remote_help
                                    : fmsg::open_in_viewer_local_help);
        ImGui::SameLine();
    }
    if (have && (e.kind == Kind::PointCloud || e.kind == Kind::Splat)) {
        if (ui::ToggleButton(fmsg::preview_points, &_show_cloud)) _preview.clear();
        ui::help_on_hover(fmsg::preview_points_help);
        ImGui::SameLine();
    }
    if (have && e.is_dir) {
        if (ui::ToggleButton(fmsg::show_photos, &_show_photos)) _preview.clear();
        ImGui::SameLine();
    }
    if (have && e.kind == Kind::Video && remote_on) {
        if (ui::Button(fmsg::play_in_browser)) remote::play_video(path);
        ui::help_on_hover(fmsg::play_in_browser_help);
        ImGui::SameLine();
    }
    if (have && host.open_here &&
        (e.is_dir || e.kind == Kind::Splat || e.kind == Kind::Mesh || e.kind == Kind::Video ||
         e.kind == Kind::PointCloud)) {
        if (ui::Button(fmsg::open_here)) host.open_here(path);
        ui::help_on_hover(fmsg::open_here_help);
        ImGui::SameLine();
    }
    if (have) {
        if (ui::Button(fmsg::copy_path)) ImGui::SetClipboardText(path.c_str());
    }
    ImGui::NewLine();

    // Moving files between this machine and the one the person sits at.
    if (remote_on) {
        if (have) {
            if (ui::Button(fmsg::download)) remote::download(path);
            ui::help_on_hover(fmsg::download_help);
            ImGui::SameLine();
        }
        if (ui::Button(fmsg::upload_here)) remote::request_upload(_cwd);
        ui::help_on_hover(fmsg::upload_help);
    } else {
        const bool busy = _copy_busy.load() || _dialog.is_open();
        ImGui::BeginDisabled(busy);
        if (have) {
            if (ui::Button(fmsg::export_to)) {
                _pick = Pick::Export;
                _dialog.open(fmsg::pick_export.get(), FileDialog::Mode::Folder);
            }
            ui::help_on_hover(fmsg::export_help);
            ImGui::SameLine();
        }
        if (ui::Button(fmsg::import_files)) {
            _pick = Pick::ImportFiles;
            _dialog.open(fmsg::pick_import_files.get(), FileDialog::Mode::File, {}, "", true);
        }
        ui::help_on_hover(fmsg::import_help);
        ImGui::SameLine();
        if (ui::Button(fmsg::import_folder)) {
            _pick = Pick::ImportFolder;
            _dialog.open(fmsg::pick_import_folder.get(), FileDialog::Mode::Folder);
        }
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ui::Button(fmsg::new_folder)) {
        _want_new_folder = true;
        _new_folder.clear();
        _new_folder_error.clear();
    }
    std::string status;
    {
        std::lock_guard<std::mutex> lk(_mu);
        status = _copy_status;
    }
    if (!status.empty()) ui::TextWrappedRaw(status);
}

void FileBrowser::draw_new_folder() {
    if (_want_new_folder) {
        ui::OpenPopup(fmsg::new_folder);
        _want_new_folder = false;
    }
    if (!ui::BeginPopupModal(fmsg::new_folder, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::SetNextItemWidth(px(320.0f));
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ui::InputText(fmsg::new_folder_name, &_new_folder,
                                     ImGuiInputTextFlags_EnterReturnsTrue);
    if (!_new_folder_error.empty()) ui::TextColoredWrappedRaw(ImVec4(1, 0.5f, 0.4f, 1), _new_folder_error);
    if (ui::Button(fmsg::create) || enter) {
        const std::string target = storage::join_inside(_cwd, _new_folder);
        std::error_code ec;
        if (target.empty() || _new_folder.find('/') != std::string::npos) {
            _new_folder_error = spirula::i18n::format(fmsg::create_failed, {_new_folder});
        } else if (!fs::create_directory(fs::u8path(target), ec) || ec) {
            _new_folder_error = spirula::i18n::format(
                fmsg::create_failed, {ec ? ec.message() : std::string("exists")});
        } else {
            ImGui::CloseCurrentPopup();
            start_listing();
        }
    }
    ImGui::SameLine();
    if (ui::Button(fmsg::cancel)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

}  // namespace gui
