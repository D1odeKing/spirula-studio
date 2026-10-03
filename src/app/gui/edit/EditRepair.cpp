// EditRepair.cpp -- the editing session's Repair tab: cameras the SfM put in
// the wrong place or left out, fixed against the rest of the model
// (sfm/Repair.h) and kept or discarded as a whole.

#include "app/gui/edit/EditSession.h"

#include "app/gui/GlLoader.h"
#include "app/gui/Ui.h"
#include "app/gui/ViewportPanel.h"
#include "core/CameraModel.h"
#include "data/DatasetParser.h"
#include "data/SparseEdit.h"
#include "i18n/catalog/Edit.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <mutex>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;
namespace msg = spirula::i18n::msg::edit;
using spirula::Sim3;
using spirula::i18n::Msg;

namespace gui {

namespace {

// The files a repair writes, which are what Keep replaces.
// Long edge of the photo panel's copy: a sidebar is a few hundred pixels.
constexpr int kPhotoSide = 1024;

constexpr const char* kModelFiles[] = {"cameras.bin", "images.bin", "points3D.bin", "rigs.txt",
                                       "gauge.txt"};

fs::path fresh_stage() {
    std::error_code ec;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path p = fs::temp_directory_path(ec) / ("spirula-repair-" + std::to_string(now));
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

// OpenGL camera-to-world (raw file frame) moved by `m`, as COLMAP's world-to-camera.
void to_colmap(const std::array<double, 12>& c2w, const Sim3& m, RepairRequest::Hint& h) {
    double R[9], C[3] = {c2w[3], c2w[7], c2w[11]}, Cm[3];
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) {
            double v = 0;
            for (int j = 0; j < 3; j++) v += m.R[r * 3 + j] * c2w[(size_t)(j * 4 + k)];
            R[r * 3 + k] = k == 0 ? v : -v;  // OpenGL -> OpenCV: y and z flip
        }
    m.apply(C, Cm);
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) h.R[r * 3 + k] = R[k * 3 + r];
    for (int r = 0; r < 3; r++)
        h.t[r] = -(h.R[r * 3 + 0] * Cm[0] + h.R[r * 3 + 1] * Cm[1] + h.R[r * 3 + 2] * Cm[2]);
}

const Msg& outcome_label(const std::string& o) {
    if (o == "moved") return msg::outcome_moved;
    if (o == "kept") return msg::outcome_kept;
    if (o == "added") return msg::outcome_added;
    if (o == "removed") return msg::outcome_removed;
    return msg::outcome_failed;
}

}  // namespace


bool EditSession::repair_available() {
    if (_repair_avail >= 0) return _repair_avail == 1;
    _repair_avail = 0;
    if (!_doc || _doc->kind() != EditDoc::Kind::Points || _doc->layer_count() < 2) return false;
    const auto& pd = static_cast<const PointsDoc&>(*_doc);
    if (pd.format() != spirula::SparseFormat::Colmap || pd.dataset_dir().empty()) return false;
    std::error_code ec;
    const fs::path ds(pd.dataset_dir());
    _repair_model = find_colmap_model(pd.dataset_dir(), "");
    if (_repair_model.empty() || !fs::exists(fs::path(_repair_model) / "images.bin", ec) ||
        !fs::exists(ds / "matches.bin", ec) || !fs::is_directory(ds / "features", ec))
        return false;
    _repair_avail = 1;
    return true;
}

bool EditSession::camera_mode() const {
    return _tab == 2 && _doc && _doc->kind() == EditDoc::Kind::Points && _doc->layer() == 1 &&
           !_doc->sel().empty();
}

void EditSession::begin_camera_move() {
    _cam_xform = camera_mode();
    if (!_cam_xform) return;
    const auto& pd = static_cast<const PointsDoc&>(*_doc);
    _cam_from = pd.moved_cameras();
    _cam_start.clear();
    const uint8_t* alive = _doc->alive();
    for (int64_t i = 0; i < _doc->count(); i++)
        if (alive[i] && _doc->sel().selected(i)) _cam_start[i] = pd.camera_pose(i);
}

// shared = base * placement * positions, positions = view_frame * raw.
PointsDoc::Poses EditSession::moved_by(const Sim3& shared_step) const {
    const Sim3 bp = base_frame() * _doc->placement();
    const Sim3 vf = _doc->view_frame();
    const Sim3 step = vf.inverse() * bp.inverse() * shared_step * bp * vf;
    PointsDoc::Poses out = _cam_from;
    for (const auto& kv : _cam_start) {
        const std::array<double, 12>& c = kv.second;
        std::array<double, 12> n;
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 3; k++) {
                double v = 0;
                for (int j = 0; j < 3; j++) v += step.R[r * 3 + j] * c[(size_t)(j * 4 + k)];
                n[(size_t)(r * 4 + k)] = v;
            }
        const double C[3] = {c[3], c[7], c[11]};
        double Cn[3];
        step.apply(C, Cn);
        for (int r = 0; r < 3; r++) n[(size_t)(r * 4 + 3)] = Cn[r];
        out[kv.first] = n;
    }
    return out;
}


// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

void EditSession::start_repair(RepairKind kind) {
    if (!_doc || busy() || !repair_available()) return;
    auto& pd = static_cast<PointsDoc&>(*_doc);
    discard_repair();

    RepairRequest rq;
    rq.workspace = pd.dataset_dir();
    rq.match = _repair_match;
    std::error_code ec;
    if (fs::is_directory(fs::path(rq.workspace) / "images", ec))
        rq.image_dir = (fs::path(rq.workspace) / "images").string();
    const spirula::SparseKeep keep = pd.sparse_keep();
    rq.exclude = keep.drop_images;
    const uint8_t* alive = pd.alive_of(1).data();
    const Selection& sel = pd.sel_of(1);
    const Sim3 moved = _doc->file_placement();
    switch (kind) {
        case RepairKind::Replace:
            for (int64_t i = 0; i < (int64_t)pd.alive_of(1).size(); i++)
                if (alive[i] && sel.selected(i)) rq.replace.push_back(pd.image_file(i));
            break;
        case RepairKind::Snap:
            for (const auto& kv : pd.moved_cameras()) {
                if (!alive[kv.first]) continue;
                RepairRequest::Hint h;
                h.name = pd.image_file(kv.first);
                to_colmap(kv.second, moved, h);
                rq.hints.push_back(std::move(h));
            }
            break;
        case RepairKind::Missing: rq.add_missing = true; break;
        case RepairKind::Audit: rq.audit_all = true; break;
    }
    if (rq.replace.empty() && rq.hints.empty() && !rq.add_missing && !rq.audit_all) return;

    // The repair starts from what is on screen: deletions and the placement
    // are written into a copy first, exactly as Save a copy would write them.
    const bool edited = !moved.is_identity() || !keep.drop_images.empty() ||
                        std::find(keep.points.begin(), keep.points.end(), 0) != keep.points.end();
    const fs::path stage = fresh_stage();
    _repair_stage = stage.string();
    _repair_moved = moved;
    rq.output_dir = (stage / "out").string();
    _repair_busy = true;
    _repair_cancel = false;
    _repair_error.clear();
    _status = msg::repair_running.get();
    _status_err = false;
    const std::string dataset = pd.dataset_dir();
    _repair_worker = std::thread([this, rq, keep, moved, edited, stage, dataset]() mutable {
        struct Done {
            std::atomic<bool>& f;
            ~Done() { f = false; }
        } done{_repair_busy};
        try {
            if (edited) {
                spirula::sparse_write_copy(dataset, (stage / "in").string(), keep, &moved);
                rq.model_dir = (stage / "in" / "sparse" / "0").string();
            } else {
                rq.model_dir = _repair_model;
            }
            std::vector<RepairResultLine> report;
            _repair_result = run_repair_in_process(
                rq,
                [this](const std::string& line) {
                    std::lock_guard<std::mutex> lk(_repair_log_mtx);
                    _repair_log.push_back(line);
                },
                _repair_cancel, &report);
            _repair_report = std::move(report);
        } catch (const std::exception& e) {
            _repair_result = InProcessResult{};
            _repair_result.exit_code = 2;
            _repair_result.error = e.what();
        }
    });
}

void EditSession::poll_repair() {
    if (_repair_busy.load() || !_repair_worker.joinable()) return;
    _repair_worker.join();
    if (_repair_result.cancelled) {
        discard_repair();
        _status.clear();
        return;
    }
    if (_repair_result.exit_code != 0) {
        const std::string why = _repair_result.error.empty()
                                    ? std::to_string(_repair_result.exit_code)
                                    : _repair_result.error;
        _status = spirula::i18n::format(msg::repair_failed, {why});
        _status_err = true;
        note(_status);
        discard_repair();
        return;
    }
    long long n[5] = {0, 0, 0, 0, 0};
    for (const RepairResultLine& l : _repair_report)
        n[l.outcome == "moved" ? 0 : l.outcome == "added" ? 1 : l.outcome == "kept" ? 2
          : l.outcome == "removed" ? 4 : 3]++;
    _status = spirula::i18n::format(msg::repair_result, {n[0], n[1], n[2], n[3], n[4]});
    _status_err = false;
    note(_status);
    _repair_shown = true;
    show_repair_preview();
}

// The repaired poses drawn over the model, each moved camera beside a ghost of
// where it was. The result was written in the staged frame: placement undone.
void EditSession::show_repair_preview() {
    if (!_doc || _doc->kind() != EditDoc::Kind::Points) return;
    auto& pd = static_cast<PointsDoc&>(*_doc);
    std::map<int32_t, ColmapImage> imgs;
    try {
        imgs = read_images_binary((fs::path(_repair_stage) / "out").string());
    } catch (const std::exception&) {
        return;
    }
    std::map<std::string, const ColmapImage*> by_name;
    for (const auto& kv : imgs) by_name[kv.second.name] = &kv.second;
    const Sim3 back = _repair_moved.inverse();
    auto pose_of = [&](const ColmapImage& im) {
        const double w = im.qvec[0], x = im.qvec[1], y = im.qvec[2], z = im.qvec[3];
        const double R[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
                             2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
                             2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)};
        double C[3], Cb[3];
        for (int k = 0; k < 3; k++)
            C[k] = -(R[0 * 3 + k] * im.tvec[0] + R[1 * 3 + k] * im.tvec[1] + R[2 * 3 + k] * im.tvec[2]);
        back.apply(C, Cb);
        std::array<double, 12> c;
        for (int r = 0; r < 3; r++) {
            for (int k = 0; k < 3; k++) {
                double v = 0;  // back.R * R^T, then OpenCV -> OpenGL
                for (int j = 0; j < 3; j++) v += back.R[r * 3 + j] * R[k * 3 + j];
                c[(size_t)(r * 4 + k)] = k == 0 ? v : -v;
            }
            c[(size_t)(r * 4 + 3)] = Cb[r];
        }
        return c;
    };
    // Report names are the model's, relative to the image folder; the dataset's are paths.
    auto camera_of = [&](const std::string& name) -> int64_t {
        std::string want = name;
        std::replace(want.begin(), want.end(), '\\', '/');
        const std::vector<uint8_t>& alive = pd.alive_of(1);
        for (int64_t i = 0; i < (int64_t)alive.size(); i++) {
            std::string f = pd.image_file(i);
            std::replace(f.begin(), f.end(), '\\', '/');
            if (f.size() >= want.size() &&
                f.compare(f.size() - want.size(), want.size(), want) == 0 &&
                (f.size() == want.size() || f[f.size() - want.size() - 1] == '/'))
                return i;
        }
        return -1;
    };
    int64_t like = 0;
    while (like < (int64_t)pd.alive_of(1).size() && !pd.alive_of(1)[(size_t)like]) like++;
    PointsDoc::RepairPreview pv;
    for (const RepairResultLine& l : _repair_report) {
        const auto it = by_name.find(l.name);
        const int64_t cam = camera_of(l.name);
        if (l.outcome == "failed" || l.outcome == "removed") {
            if (cam >= 0) pv.marks[cam] = PointsDoc::Mark::Failed;
        } else if (it != by_name.end() && cam >= 0 && l.outcome == "moved") {
            pv.poses[cam] = pose_of(*it->second);
            pv.marks[cam] = PointsDoc::Mark::Moved;
        } else if (it != by_name.end() && cam < 0 && l.outcome == "added") {
            pv.added.push_back({l.name, like, pose_of(*it->second)});
        }
    }
    pd.set_repair_preview(std::move(pv));
}

void EditSession::keep_repair() {
    if (!_repair_shown || _repair_model.empty()) return;
    const fs::path out = fs::path(_repair_stage) / "out";
    const fs::path dst(_repair_model);
    std::error_code ec;
    for (const char* f : kModelFiles) {
        if (!fs::exists(out / f, ec)) continue;
        const fs::path orig = dst / (std::string(f) + ".orig");
        if (fs::exists(dst / f, ec) && !fs::exists(orig, ec)) fs::copy_file(dst / f, orig, ec);
        fs::copy_file(out / f, dst / f, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            _status = spirula::i18n::format(msg::repair_failed, {ec.message()});
            _status_err = true;
            return;
        }
    }
    _status = spirula::i18n::format(msg::repair_wrote, {_repair_model});
    _status_err = false;
    note(_status);
    discard_repair();
    _saved_over_source = true;
    _doc->mark_saved();
    if (_on_model_replaced) _on_model_replaced();
}

void EditSession::discard_repair() {
    if (_doc && _doc->kind() == EditDoc::Kind::Points &&
        !static_cast<PointsDoc&>(*_doc).repair_preview().empty())
        static_cast<PointsDoc&>(*_doc).set_repair_preview({});
    _repair_shown = false;
    _repair_report.clear();
    if (!_repair_stage.empty()) {
        std::error_code ec;
        fs::remove_all(_repair_stage, ec);
        _repair_stage.clear();
    }
}


// ---------------------------------------------------------------------------
// The camera's photograph
// ---------------------------------------------------------------------------

void EditSession::step_camera(int dir) {
    const std::vector<uint8_t>& alive = _doc->alive_of(1);
    const int64_t n = (int64_t)alive.size();
    if (n == 0) return;
    int64_t at = -1;
    for (int64_t i = 0; i < n && at < 0; i++)
        if (alive[(size_t)i] && _doc->sel().selected(i)) at = i;
    for (int64_t k = 1; k <= n; k++) {
        const int64_t i = ((at < 0 ? (dir > 0 ? -1 : 0) : at) + dir * k % n + n) % n;
        if (!alive[(size_t)i]) continue;
        std::vector<uint8_t> w((size_t)n, 0);
        w[(size_t)i] = 255;
        run_select(std::move(w), (dir > 0 ? msg::photo_next : msg::photo_prev).get());
        return;
    }
}

// The navigation camera put where camera `i` stands, with its lens: what the
// model looks like from there, beside its photograph, is what shows it wrong.
void EditSession::look_through(int64_t i) {
    if (!_panel) return;
    const auto& pd = static_cast<const PointsDoc&>(*_doc);
    const ParsedDataset& ds = pd.parsed();
    const std::array<double, 12> c = pd.camera_pose(i);
    const Sim3 to_shared = base_frame() * _doc->placement() * _doc->view_frame();
    float pose[12], target[3];
    const double C[3] = {c[3], c[7], c[11]};
    double Cs[3];
    to_shared.apply(C, Cs);
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 3; k++) {
            double v = 0;
            for (int j = 0; j < 3; j++) v += to_shared.R[r * 3 + j] * c[(size_t)(j * 4 + k)];
            pose[r * 4 + k] = (float)v;
        }
        pose[r * 4 + 3] = (float)Cs[r];
    }
    const double reach = 0.5 * (double)_doc->extent() * (base_frame() * _doc->placement()).s;
    for (int r = 0; r < 3; r++) target[r] = (float)(Cs[r] - pose[r * 4 + 2] * reach);
    _panel->set_nav_pose(pose, target);
    const int model = ds.camera_models.empty() ? 0 : (int)ds.camera_models[(size_t)i];
    const double W = ds.widths[(size_t)i], fx = std::max(1e-6f, ds.intrins[(size_t)i * 4]);
    double fov = 0;
    switch ((CameraModelType)model) {
        case CameraModelType::PINHOLE: fov = 2 * std::atan(W / (2 * fx)); break;
        case CameraModelType::FISHEYE: fov = W / fx; break;
        case CameraModelType::EQUISOLID: fov = 4 * std::asin(std::min(1.0, W / (4 * fx))); break;
        case CameraModelType::EQUIRECTANGULAR: fov = 0; break;
    }
    _panel->set_view_lens(model, (float)(fov * 180.0 / M_PI));
    _panel->invalidate();
}

void EditSession::draw_camera_photo(float full) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float third = (full - st.ItemSpacing.x * 2) / 3.0f;
    auto& pd = static_cast<PointsDoc&>(*_doc);
    const std::vector<uint8_t>& alive = pd.alive_of(1);
    int64_t cam = -1;
    if (_doc->layer() == 1)
        for (int64_t i = 0; i < (int64_t)alive.size() && cam < 0; i++)
            if (alive[(size_t)i] && _doc->sel().selected(i)) cam = i;

    ui::SeparatorText(msg::sec_camera_photo);
    ui::TextDisabledWrapped(msg::photo_legend);
    if (cam < 0) {
        ui::TextDisabledWrapped(msg::photo_none);
        return;
    }

    // One load at a time; the newest selection is fetched once it finishes.
    _photo.want = cam;
    if (!_photo.busy.load() && _photo.worker.joinable()) _photo.worker.join();
    if (!_photo.busy.load() && _photo.loaded != _photo.want) {
        _photo.busy = true;
        const std::string path = pd.image_file(cam);
        const int64_t id = cam;
        _photo.worker = std::thread([this, path, id] {
            Picture p;
            const bool ok = load_picture(path, "", kPhotoSide, p);
            {
                std::lock_guard<std::mutex> lk(_photo.mu);
                if (!ok) p = Picture{};
                _photo.pic = std::move(p);
                _photo.loaded = id;
            }
            _photo.busy = false;
        });
    }
    {
        std::lock_guard<std::mutex> lk(_photo.mu);
        if (_photo.shown != _photo.loaded) {
            _photo.shown = _photo.loaded;
            _photo.w = _photo.pic.w;
            _photo.h = _photo.pic.h;
            if (!_photo.pic.empty()) {
                GLuint tex = _photo.tex;
                if (!tex) glGenTextures(1, &tex);
                _photo.tex = tex;
                glBindTexture(GL_TEXTURE_2D, tex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, _photo.w, _photo.h, 0, GL_RGB,
                             GL_UNSIGNED_BYTE, _photo.pic.rgb.data());
            }
        }
    }

    ui::TextRaw(fs::path(pd.image_file(cam)).filename().string());
    const PointsDoc::RepairPreview& pv = pd.repair_preview();
    const auto mark = pv.marks.find(cam);
    if (mark != pv.marks.end())
        ui::TextColoredWrapped(mark->second == PointsDoc::Mark::Failed ? ImVec4(1, 0.4f, 0.4f, 1)
                                                                       : ImVec4(0.4f, 1, 0.5f, 1),
                               mark->second == PointsDoc::Mark::Failed ? msg::photo_failed
                                                                       : msg::photo_repaired);
    else if (pd.moved_cameras().count(cam))
        ui::TextColoredWrapped(ImVec4(1, 0.9f, 0.2f, 1), msg::photo_hand_moved);

    if (_photo.shown == cam && _photo.tex && _photo.w > 0) {
        const float h = full * (float)_photo.h / (float)_photo.w;
        ImGui::Image((ImTextureID)(intptr_t)_photo.tex, ImVec2(full, h));
    } else {
        ui::TextDisabled(msg::photo_loading);
    }
    if (ui::Button(msg::photo_prev, ImVec2(third, 0))) step_camera(-1);
    ImGui::SameLine();
    if (ui::Button(msg::photo_look, ImVec2(third, 0))) look_through(cam);
    ui::help_on_hover(msg::photo_look_help);
    ImGui::SameLine();
    if (ui::Button(msg::photo_next, ImVec2(third, 0))) step_camera(1);
}


// ---------------------------------------------------------------------------
// The tab
// ---------------------------------------------------------------------------

void EditSession::draw_repair_tab(float full) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float half = (full - st.ItemSpacing.x) * 0.5f;
    ui::TextDisabledWrapped(msg::repair_help);
    if (!repair_available()) {
        ui::TextDisabledWrapped(msg::repair_unavailable);
        return;
    }
    EditDoc& d = *_doc;
    auto& pd = static_cast<PointsDoc&>(d);

    if (_repair_shown) {
        ImGui::BeginChild("##repairres",
                          ImVec2(0, (float)std::clamp((int)_repair_report.size() + 1, 3, 10) *
                                        ImGui::GetTextLineHeightWithSpacing()),
                          ImGuiChildFlags_Borders);
        for (const RepairResultLine& l : _repair_report)
            ui::TextRaw(fs::path(l.name).filename().string() + "  " +
                        outcome_label(l.outcome).get());
        ImGui::EndChild();
        if (ui::Button(msg::repair_keep, ImVec2(half, 0))) keep_repair();
        ui::help_on_hover(msg::repair_keep_help, {_repair_model});
        ImGui::SameLine();
        if (ui::Button(msg::repair_discard, ImVec2(half, 0))) discard_repair();
        if (_repair_shown) draw_camera_photo(full);
        return;
    }

    // Selection-driven actions want the camera layer under the tools.
    const bool cams = d.layer() == 1;
    const bool picked = cams && !d.sel().empty();
    ImGui::BeginDisabled(!picked);
    if (ui::Button(msg::repair_replace, ImVec2(full, 0))) start_repair(RepairKind::Replace);
    ImGui::EndDisabled();
    ui::help_on_hover(msg::repair_replace_help);

    ImGui::BeginDisabled(!picked);
    if (ui::Button(msg::repair_move, ImVec2(full, 0))) {
        if (_tool.id() != ToolId::Transform) _xform_return = _tool.id();
        _tool.set_id(ToolId::Transform);
    }
    ImGui::EndDisabled();
    ui::help_on_hover(msg::repair_move_help);
    const size_t moved = pd.moved_cameras().size();
    if (moved) {
        ui::Text(msg::repair_moved_count, {(long long)moved});
        if (ui::Button(msg::repair_snap, ImVec2(half, 0))) start_repair(RepairKind::Snap);
        ui::help_on_hover(msg::repair_snap_help);
        ImGui::SameLine();
        if (ui::Button(msg::repair_forget_moves, ImVec2(half, 0)))
            d.run(make_camera_move_op(pd, {}, msg::repair_forget_moves.get()));
    }

    ImGui::Spacing();
    if (ui::Button(msg::repair_missing, ImVec2(full, 0))) start_repair(RepairKind::Missing);
    ui::help_on_hover(msg::repair_missing_help);
    if (ui::Button(msg::repair_audit, ImVec2(full, 0))) start_repair(RepairKind::Audit);
    ui::help_on_hover(msg::repair_audit_help);
    ui::Checkbox(msg::repair_match, &_repair_match);
    ui::help_on_hover(msg::repair_match_help);
    draw_camera_photo(full);
}

}  // namespace gui
