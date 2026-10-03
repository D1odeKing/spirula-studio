// EditRepair.cpp -- the editing session's Repair tab: cameras the SfM put in
// the wrong place or left out, fixed against the rest of the model
// (sfm/Repair.h) and kept or discarded as a whole.

#include "app/gui/edit/EditSession.h"

#include "app/gui/Ui.h"
#include "data/DatasetParser.h"
#include "data/SparseEdit.h"
#include "i18n/catalog/Edit.h"

#include "imgui.h"

#include <algorithm>
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
    _repair_shown = false;
    _repair_report.clear();
    if (!_repair_stage.empty()) {
        std::error_code ec;
        fs::remove_all(_repair_stage, ec);
        _repair_stage.clear();
    }
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
}

}  // namespace gui
