// MeshLabPanel.cpp -- see MeshLabPanel.h.

#include "app/gui/MeshLabPanel.h"

#include "app/gui/Layout.h"
#include "app/gui/Ui.h"
#include "checkpoint/SplatPly.h"
#include "data/Json.h"
#include "i18n/catalog/MeshLab.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <tuple>

namespace fs = std::filesystem;

namespace gui {

namespace M = spirula::i18n::msg::meshlab;

namespace {

// The run directory of the job's model, "" when it has none.
std::string run_dir_of(const MeshJob& job) {
    try {
        return spirula::find_splat_ply(job.checkpoint).second;
    } catch (const std::exception&) {
        return {};
    }
}

// The dataset a dense cloud picked as the model belongs to: <dataset>/dense/roma.ply,
// or a generation's copy under dense/generations/. "" for anything else.
std::string dataset_of_cloud(const std::string& model) {
    std::error_code ec;
    const fs::path p(model);
    if (p.extension() != ".ply" || !fs::is_regular_file(p, ec)) return {};
    for (fs::path d = p.parent_path(); !d.empty() && d != d.root_path(); d = d.parent_path())
        if (d.filename() == "dense") return d.parent_path().string();
    return {};
}

void add_map_dirs(const MeshJob& job, std::vector<std::string>& argv) {
    if (!job.normal_dir.empty()) argv.insert(argv.end(), {"--normals", job.normal_dir});
    if (!job.depth_dir.empty()) argv.insert(argv.end(), {"--depths", job.depth_dir});
}

std::string number(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

}  // namespace

void draw_meshlab_options(MeshJob& job) {
    ImGui::SetNextItemWidth(px(220.0f));
    ui::Combo(M::gui_source, &job.source, {&M::gui_source_splats, &M::gui_source_dense});
    ui::help_on_hover(M::gui_source_help);
    ui::Checkbox(M::gui_refine, &job.refine_normals);
    ui::help_on_hover(M::gui_refine_help);
    if (job.refine_normals) {
        ImGui::Indent();
        ImGui::SetNextItemWidth(px(120.0f));
        ui::InputFloat(M::gui_max_shift, &job.max_shift, 0.0f, 0.0f, "%.4f");
        job.max_shift = std::max(0.0f, job.max_shift);
        ui::help_on_hover(M::gui_max_shift_help);
        ImGui::Unindent();
    }
    if (job.source == 1 || job.refine_normals) {
        ImGui::Indent();
        for (auto [dir, label, id] : {std::tuple{&job.normal_dir, &M::gui_normals_dir, "##meshlabnormals"},
                                      std::tuple{&job.depth_dir, &M::gui_depths_dir, "##meshlabdepths"}}) {
            ImGui::SetNextItemWidth(px(420.0f));
            ui::InputTextWithHintRaw(id, M::gui_maps_help, dir);
            ImGui::SameLine();
            ui::TextDisabled(*label);
            ui::help_on_hover(M::gui_maps_help);
        }
        ImGui::Unindent();
    }
    if (meshlab_no_texture(job)) {
        job.colors[2] = false;
        ui::TextDisabled(M::gui_no_texture);
    }
}

bool meshlab_no_texture(const MeshJob& job) { return job.source == 1 || job.refine_normals; }

std::string meshlab_dataset(const MeshJob& job) {
    // A cloud belongs to the dataset it sits in, whatever the photos folder says.
    const std::string from_cloud = dataset_of_cloud(job.checkpoint);
    if (!from_cloud.empty()) return from_cloud;
    if (!job.data_dir.empty()) return job.data_dir;
    const std::string run = run_dir_of(job);
    if (run.empty()) return {};
    try {
        const JsonValue cfg = json_parse_file((fs::path(run) / "config.json").string());
        const JsonValue* data = cfg.find("data");
        if (data && !data->is_null()) return data->as_string();
    } catch (const std::exception&) {
    }
    return {};
}

bool meshlab_dense_argv(const MeshJob& job, const std::string& formats, const std::string& colors,
                        std::vector<std::string>& argv) {
    const std::string dataset = meshlab_dataset(job);
    if (dataset.empty() || argv.size() < 3) return false;
    argv.resize(3);
    argv.insert(argv.end(), {"mesh-dense", "--dataset", dataset, "--format", formats,
                             "--color", colors.find("vertex") != std::string::npos ? "vertex" : "none",
                             "--output", job.output});
    if (!dataset_of_cloud(job.checkpoint).empty()) argv.insert(argv.end(), {"--cloud", job.checkpoint});
    add_map_dirs(job, argv);
    return true;
}

int meshlab_refine(const MeshJob& job, const std::vector<std::string>& prefix, const std::vector<std::string>& made,
                   const std::function<int(const std::vector<std::string>&)>& run) {
    const std::string dataset = meshlab_dataset(job);
    if (dataset.empty()) return 1;
    // A splat mesh is in its run's training frame and goes back there, where
    // the preview draws it over the model; the dense mesher writes the file frame.
    const std::string rd = job.source == 0 ? run_dir_of(job) : std::string();
    const bool framed = !rd.empty() && fs::is_regular_file(fs::path(rd) / "scene_transform.json");
    // STL keeps no shared vertices, so every triangle would turn on its own and
    // the mesh tear: each output is refined from the PLY, which is done last.
    std::vector<std::string> order = made;
    auto ply = std::find_if(order.begin(), order.end(), [](const std::string& f) { return fs::path(f).extension() == ".ply"; });
    const std::string from = ply != order.end() ? *ply : std::string();
    if (ply != order.end()) std::rotate(ply, ply + 1, order.end());
    for (const std::string& file : order) {
        std::vector<std::string> argv = prefix;
        argv.insert(argv.end(), {"mesh-refine", "--mesh", from.empty() ? file : from, "--dataset", dataset, "-o", file});
        if (framed) argv.insert(argv.end(), {"--run", rd, "--keep-frame"});
        if (job.max_shift > 0) argv.insert(argv.end(), {"--max-shift", number(job.max_shift)});
        add_map_dirs(job, argv);
        const int rc = run(argv);
        if (rc != 0) return rc;
    }
    return 0;
}

}  // namespace gui
