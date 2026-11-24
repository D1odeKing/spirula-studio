// MeasureTool.cpp -- see MeasureTool.h.

#include "app/gui/MeasureTool.h"

#include "app/gui/Layout.h"
#include "app/gui/Ui.h"
#include "app/gui/ViewportPanel.h"
#include "app/gui/edit/SelectShape.h"
#include "data/DatasetParser.h"
#include "data/DatasetRescale.h"
#include "i18n/catalog/Gui.h"
#include "i18n/catalog/MeshLab.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>

namespace gui {

namespace M = spirula::i18n::msg::meshlab;

namespace {

std::string number(double v, const char* fmt = "%.5g") {
    char buf[32];
    std::snprintf(buf, sizeof buf, fmt, v);
    return buf;
}

// Normalized (what the viewers navigate) -> train -> raw -> file.
void normalized_to_file(const ParsedDataset& ds, const float n[3], double out[3]) {
    const auto& T = ds.train_to_normalized;   // normalized -> train, despite the name
    double raw[3];
    for (int r = 0; r < 3; r++)
        raw[r] = T[r*4+0]*n[0] + T[r*4+1]*n[1] + T[r*4+2]*n[2] + T[r*4+3] + ds.center[r];
    const auto& F = ds.raw_to_file;
    for (int r = 0; r < 3; r++)
        out[r] = F[r*4+0]*raw[0] + F[r*4+1]*raw[1] + F[r*4+2]*raw[2] + F[r*4+3];
}

bool view(const ViewportPanel& panel, ViewProjection& out) {
    float x, y, w, h;
    panel.image_rect(x, y, w, h);
    if (w < 8.0f || h < 8.0f) return false;
    out.W = (int)w;
    out.H = (int)h;
    panel.view_camera(out.W, out.H, out.w2c, out.fx, out.fy, out.camera_model, out.eye);
    out.cx = 0.5f * (float)out.W;
    out.cy = 0.5f * (float)out.H;
    out.ortho_back = panel.ortho_pullback(false);
    return true;
}

}  // namespace

double MeasureTool::length() const {
    if (_count < 2 || !_ds) return -1.0;
    double a[3], b[3];
    normalized_to_file(*_ds, _pts[0], a);
    normalized_to_file(*_ds, _pts[1], b);
    return std::sqrt((a[0]-b[0])*(a[0]-b[0]) + (a[1]-b[1])*(a[1]-b[1]) + (a[2]-b[2])*(a[2]-b[2]));
}

std::vector<std::string> MeasureTool::take_log() {
    std::vector<std::string> out;
    out.swap(_log);
    return out;
}

bool MeasureTool::draw_toolbar(ViewportPanel& panel, const ParsedDataset* ds,
                               const std::string& dataset, bool can_rescale) {
    if (dataset != _dataset) {
        _dataset = dataset;
        _count = 0;
        _waiting = false;
    }
    _panel = &panel;
    _ds = ds;

    bool finished = false;
    if (_job.valid() && _job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const std::string err = _job.get();
        if (err.empty()) {
            _log.push_back(spirula::i18n::format(M::gui_measure_done, {_dataset, number(_scale, "%.6g")}));
            finished = true;
        } else {
            _log.push_back(spirula::i18n::format(M::gui_measure_failed, {err}));
        }
    }

    ImGui::BeginDisabled(!ds);
    if (ui::Checkbox(M::gui_measure, &_on)) {
        panel.set_interactor(_on ? this : nullptr);
        _waiting = false;
        panel.invalidate();
    }
    ImGui::EndDisabled();
    ui::help_on_hover(M::gui_measure_help);
    if (_on && ds) {
        ImGui::SameLine();
        const double d = length();
        if (d < 0) {
            ui::TextDisabled(M::gui_measure_hint);
        } else {
            ui::TextRaw(spirula::i18n::format(ds->gauge_metric ? M::gui_measure_metres : M::gui_measure_units,
                                              {number(d)}));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(px(90.0f));
            ui::InputDouble(M::gui_measure_true, &_truth, "%.4f");
            _truth = std::max(0.0, _truth);
            ImGui::SameLine();
            ImGui::BeginDisabled(!can_rescale || _truth <= 0 || d <= 0 || _job.valid());
            if (ui::Button(M::gui_measure_set)) {
                _scale = _truth / d;
                _confirm_open = true;
            }
            ImGui::EndDisabled();
        }
    }
    if (_job.valid()) {
        ImGui::SameLine();
        ui::TextDisabled(M::gui_measure_running);
    }
    draw_confirm();
    return finished;
}

void MeasureTool::draw_confirm() {
    if (_confirm_open) {
        ui::OpenPopup(M::gui_measure_title);
        _confirm_open = false;
        _confirm_shown = true;
    }
    if (!_confirm_shown) return;
    if (!ui::BeginPopupModal(M::gui_measure_title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        _confirm_shown = false;
        return;
    }
    ImGui::PushTextWrapPos(px(460.0f));
    ui::TextWrapped(M::gui_measure_confirm, {_dataset, number(_scale, "%.6g"), number(_truth)});
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (ui::Button(M::gui_measure_apply, ImVec2(px(150.0f), 0))) {
        const std::string dir = _dataset;
        const double s = _scale;
        _job = std::async(std::launch::async, [dir, s]() -> std::string {
            try {
                spirula::RescaleOptions opt;
                opt.apply = true;
                spirula::RescaleReport report;
                spirula::rescale_dataset(dir, s, 0.0, opt, report);
                return {};
            } catch (const std::exception& e) {
                return *e.what() ? e.what() : "?";
            }
        });
        _confirm_shown = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ui::Button(spirula::i18n::msg::gui::cancel, ImVec2(px(150.0f), 0))) {
        _confirm_shown = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

bool MeasureTool::on_viewport_input(const ViewportInput& in) {
    if (!_on || !_panel || !in.hovered || !in.clicked || _waiting) return false;
    _panel->request_pick(in.x / (float)std::max(in.W, 1), in.y / (float)std::max(in.H, 1));
    _waiting = true;
    return true;
}

void MeasureTool::draw_viewport_overlay(const ViewportOverlay& v) {
    if (!_panel) return;
    float shared[3];
    bool hit = false;
    if (_waiting && _panel->take_pick(shared, hit)) {
        _waiting = false;
        if (!hit) {
            _log.push_back(M::gui_measure_missed.get());
        } else {
            if (_count == 2) _count = 0;
            // Shared -> model: the inverse of the panel's similarity [sR | t].
            float S[12];
            _panel->model_to_shared(S);
            const float s2 = S[0]*S[0] + S[4]*S[4] + S[8]*S[8];
            const float e[3] = {shared[0] - S[3], shared[1] - S[7], shared[2] - S[11]};
            for (int r = 0; r < 3; r++)
                _pts[_count][r] = (S[0*4+r]*e[0] + S[1*4+r]*e[1] + S[2*4+r]*e[2]) / s2;
            _count++;
        }
    }
    if (!_on || _count == 0) return;

    ViewProjection cam;
    if (!view(*_panel, cam)) return;
    ImDrawList* dl = v.dl;
    ImVec2 at[2];
    bool ok[2] = {false, false};
    for (int i = 0; i < _count; i++) {
        float x, y, depth;
        ok[i] = cam.project(_pts[i], x, y, depth) && depth > 0;
        at[i] = ImVec2(v.x + x, v.y + y);
    }
    const ImU32 ink = IM_COL32(255, 210, 60, 255), shade = IM_COL32(20, 20, 20, 200);
    if (_count == 2 && ok[0] && ok[1]) {
        dl->AddLine(at[0], at[1], shade, px(4.0f));
        dl->AddLine(at[0], at[1], ink, px(2.0f));
    }
    for (int i = 0; i < _count; i++) {
        if (!ok[i]) continue;
        dl->AddCircleFilled(at[i], px(6.0f), shade);
        dl->AddCircleFilled(at[i], px(4.0f), ink);
    }
    const double d = length();
    if (d >= 0 && ok[0] && ok[1] && _ds) {
        const std::string label = spirula::i18n::format(
            _ds->gauge_metric ? M::gui_measure_metres : M::gui_measure_units, {number(d)});
        const ImVec2 size = ImGui::CalcTextSize(label.c_str());
        const ImVec2 mid((at[0].x + at[1].x) * 0.5f + px(8.0f), (at[0].y + at[1].y) * 0.5f - size.y - px(6.0f));
        dl->AddRectFilled(ImVec2(mid.x - px(4.0f), mid.y - px(2.0f)),
                          ImVec2(mid.x + size.x + px(4.0f), mid.y + size.y + px(2.0f)), shade, px(3.0f));
        dl->AddText(mid, ink, label.c_str());
    }
}

}  // namespace gui
