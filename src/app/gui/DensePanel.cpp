#include "app/gui/GuiApp.h"

#include "app/gui/SfmProgress.h"
#include "app/gui/Ui.h"
#include "dense/Artifact.h"
#include "dense/ConfigFields.h"
#include "i18n/catalog/Dense.h"
#include "i18n/catalog/Dataset.h"
#include "sfm/core/HostMemory.h"

#include <algorithm>
#include <cmath>

namespace gui {

bool GuiApp::dense_model_missing() const {
    return _dense.enable && _dense.config.checkpoint == "romav2.0.1" && !model_is_cached(dense_model_entry());
}

void GuiApp::request_dense_download() {
    if (std::find(_accepted_licenses.begin(), _accepted_licenses.end(), "roma") == _accepted_licenses.end()) {
        _license_prompt = "roma"; _license_model_id = "romav2.0.1"; _license_detector_id.clear(); _license_tick = false;
    } else _dense_download.start(dense_model_entry());
}

// While the dense step runs, and once after it for the fused cloud's final snapshot.
void GuiApp::poll_dense_progress() {
    const bool running = dataset_busy() && dataset_steps()->current() == Stage::Dense;
    if (running) _dense_live = true;
    if ((!running && !_dense_live) || _workspace.empty()) return;
    if (!_show_preview) {
        if (_dense_model_read.valid() && _dense_model_read.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            _dense_model_read.get();
        return;
    }
    const double now = ImGui::GetTime();
    if (_dense_model_read.valid()) {
        if (_dense_model_read.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        auto read = _dense_model_read.get();
        const auto upload_start = std::chrono::steady_clock::now();
        _dense_preview_error = std::move(read.error);
        if (read.updated) {
            try {
                float up[3] = {0, 0, 1};
                if (!read.model.ds.gauge_oriented && snapshot_up(read.model, up)) _model_view.set_nav_up(up);
                _live_model = std::move(read.model);
                _model_view.attach_preview_data(_live_model.ds, _live_model.post, "dense-live", 1.0f, true);
                _model_attached = true;
                _dense_model_mtime = read.stamp;
            } catch (const std::exception& e) {
                _dense_preview_error = e.what();
                _model_view.detach(); _model_view.destroy_gl();
                _live_model = LiveModel{}; _model_attached = false;
            }
        }
        // Loading and upload together consume at most about 5% of refresh time.
        const double upload_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - upload_start).count();
        _dense_refresh_seconds = std::max(0.5, (read.seconds + upload_seconds) * 20.0);
        _dense_polled_at = ImGui::GetTime();
        if (!running && !read.during_run && _dense_preview_error.empty()) _dense_live = false;
        return;
    }
    if (!_dense_preview_error.empty()) return;
    if (_dense_polled_at > 0.0 && now - _dense_polled_at < _dense_refresh_seconds) return;
    _dense_polled_at = now;
    const auto dir = spirula::dense::progress_dir(_workspace).string();
    const auto stamp = _dense_model_mtime;
    _dense_model_read = std::async(std::launch::async, [dir, stamp, running] {
        const auto start = std::chrono::steady_clock::now();
        DenseLiveRead read; read.stamp = stamp; read.during_run = running;
        try {
            const uint64_t available = sfm::availableRamBytes();
            read.updated = read_live_model(dir, read.stamp, read.model, available - available / 5);
        } catch (const std::exception& e) { read.error = e.what(); }
        read.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return read;
    });
}

void GuiApp::draw_dense_options() {
    namespace D = spirula::i18n::msg::dense;
    ui::SeparatorText(D::title);
    const auto unavailable = dense_availability();
    if (!unavailable.empty()) { ui::TextDisabledRaw(unavailable); return; }
    ui::Checkbox(D::enable, &_dense.enable);
    ui::help_on_hover_disabled(D::enable_help);
    if (!_dense.enable) return;
    ui::Checkbox(D::use_training, &_dense.use_for_training);
    ui::help_on_hover_disabled(D::use_training_help);
    ui::Checkbox(D::log_performance, &_dense.log_performance);
    ui::help_on_hover_disabled(D::log_performance_help);
    auto& config = _dense.config;
    bool changed = false;
    int matching_space = config.matching_space == "source" ? 1 : 0;
    if (ui::Combo(D::matching_space,&matching_space,{&D::rectified_space,&D::source_space})) {
        config.matching_space = matching_space ? "source" : "rectified"; changed = true;
    }
    ui::help_on_hover_disabled(D::matching_space_help);
    if (ui::Button(D::source_workflow)) { config.apply_source_workflow(); changed = true; }
    ui::help_on_hover_disabled(D::source_workflow_help);
    ImGui::BeginDisabled(config.matching_space != "source");
    ui::Text(D::reference_fraction);
    ui::help_on_hover_disabled(D::reference_fraction_help);
    changed |= ui::InputDoubleRaw("##dense-reference-fraction",&config.reference_fraction,0.05,0.1,"%.3f");
    ui::help_on_hover_disabled(D::reference_fraction_help);
    auto sample_control = [&](const spirula::i18n::Msg& label,const spirula::i18n::Msg& help,const char* id,uint64_t& setting) {
        ui::Text(label);
        ui::help_on_hover_disabled(help);
        double value = (double)setting;
        if (ui::InputDoubleRaw(id,&value,1,100,"%.0f")) {
            try { json_field::assign(setting,json_parse(json_number_exact(value))); changed = true; _dense_config_error.clear(); }
            catch (const std::exception& e) { _dense_config_error = spirula::i18n::format(D::error,{e.what()}); }
        }
        ui::help_on_hover_disabled(help);
    };
    sample_control(D::samples_per_reference,D::samples_per_reference_help,"##dense-samples",config.samples_per_reference);
    sample_control(D::sampling_seed,D::sampling_seed_help,"##dense-seed",config.sampling_seed);
    ui::Text(D::source_reprojection);
    ui::help_on_hover_disabled(D::source_reprojection_help);
    changed |= ui::InputDoubleRaw("##dense-source-error",&config.source_reprojection_error,0.1,0.5,"%.2f");
    ui::help_on_hover_disabled(D::source_reprojection_help);
    ImGui::EndDisabled();
    changed |= ui::Checkbox(D::use_masks, &config.use_masks);
    ui::help_on_hover_disabled(D::use_masks_help);
    changed |= ui::Checkbox(D::sparse_face_pairs, &config.sparse_face_pairs);
    ui::help_on_hover_disabled(D::sparse_face_pairs_help);
    // Shown in GB, stored in bytes; whole MiB keep a typed value from drifting.
    float cache_gb = (float)((double)config.image_cache_bytes / (1024.0 * 1024.0 * 1024.0));
    if (ui::InputFloat(D::image_cache, &cache_gb, 0.5f, 1.0f, "%.1f")) {
        const double mib = std::round(std::max(0.0, (double)cache_gb) * 1024.0);
        config.image_cache_bytes = (uint64_t)mib << 20; changed = true;
    }
    ui::help_on_hover_disabled(D::image_cache_help);
    if (_dense.use_for_training) draw_train_mask_mode(ImGui::GetContentRegionAvail().x * 0.55f);
    int precision = (int)config.match.precision;
    if (ui::Combo(D::precision, &precision, {&D::precision_auto, &D::precision_float32, &D::precision_mixed})) {
        config.match.precision = (spirula::roma::InferencePrecision)precision; changed = true;
    }
    ui::help_on_hover_disabled(D::precision_help);
    if (ui::BeginCombo(D::preset, config.preset.c_str())) {
        for (const char* name : {"turbo", "fast", "base", "precise", "custom"})
            if (ui::SelectableRaw(name, config.preset == name)) { config.apply_preset(name); changed = true; }
        ImGui::EndCombo();
    }
    ui::help_on_hover_disabled(D::preset_help);
    ui::Text(D::checkpoint);
    ui::help_on_hover_disabled(D::checkpoint_help);
    changed |= ui::InputTextRaw("##dense-checkpoint", &config.checkpoint);
    ui::help_on_hover_disabled(D::checkpoint_help);
    if (dense_model_missing()) {
        if (_dense_download.state() == FileDownload::State::Running) {
            ui::ProgressBarRaw(std::max(0.f, _dense_download.progress()), ImVec2(-1, 0), _dense_download.status().c_str());
            if (ui::Button(spirula::i18n::msg::dataset::cancel)) _dense_download.cancel();
            ui::help_on_hover_disabled(D::download_cancel_help);
        } else {
            if (ui::Button(spirula::i18n::msg::dataset::license_download)) request_dense_download();
            ui::help_on_hover_disabled(D::download_help);
            if (_dense_download.state() == FileDownload::State::Failed) ui::TextWrappedRaw(_dense_download.status());
        }
    }
    ui::Text(D::low_resolution);
    ui::help_on_hover_disabled(D::low_resolution_help);
    int low[2] = {config.match.low_width, config.match.low_height};
    if (ui::InputInt2Raw("##dense-low", low)) {
        config.match.low_width = low[0]; config.match.low_height = low[1]; config.preset = "custom"; changed = true;
    }
    ui::help_on_hover_disabled(D::low_resolution_help);
    ui::Text(D::high_resolution);
    ui::help_on_hover_disabled(D::high_resolution_help);
    int high[2] = {config.match.high_width, config.match.high_height};
    if (ui::InputInt2Raw("##dense-high", high)) {
        config.match.high_width = high[0]; config.match.high_height = high[1]; config.preset = "custom"; changed = true;
    }
    ui::help_on_hover_disabled(D::high_resolution_help);
    changed |= ui::Checkbox(D::bidirectional, &config.match.bidirectional);
    ui::help_on_hover_disabled(D::bidirectional_help);
    if (!config.effective_cycle_check()) ui::TextDisabledWrapped(D::cycle_dependency);
    changed |= ui::InputInt(D::neighbors, &config.pairs.neighbors);
    ui::help_on_hover_disabled(D::neighbors_help);
    changed |= ui::InputInt(D::support, &config.geometry.min_source_images);
    ui::help_on_hover_disabled(D::support_help);
    changed |= ui::InputInt(D::stride, &config.stride);
    ui::help_on_hover_disabled(D::stride_help);
    if (changed || _dense_config_text.empty()) _dense_config_text = spirula::dense::config_json(config);
    const bool advanced = ui::CollapsingHeader(D::advanced);
    ui::help_on_hover_disabled(D::advanced_help);
    if (advanced) {
        ui::InputTextMultilineRaw("##dense-json", &_dense_config_text, ImVec2(-1, 300));
        ui::help_on_hover_disabled(D::advanced_help);
        if (ui::Button(D::apply)) {
            try {
                auto edited = config;
                spirula::dense::read_config(edited, json_parse(_dense_config_text));
                config = std::move(edited); _dense_config_error.clear();
            } catch (const std::exception& e) { _dense_config_error = spirula::i18n::format(D::error, {e.what()}); }
        }
        ui::help_on_hover_disabled(D::apply_help);
        if (!_dense_config_error.empty()) ui::TextWrappedRaw(_dense_config_error);
    }
    if (!_workspace.empty() && dense_completed(_workspace) && !dataset_busy()) {
        if (ui::Button(D::preview)) request_open_splat(spirula::dense::artifact_files(_workspace).cloud.string());
        ui::help_on_hover_disabled(D::preview_help);
    }
}

}  // namespace gui
