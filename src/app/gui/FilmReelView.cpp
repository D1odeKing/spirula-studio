// FilmReelView.cpp -- FilmReel on screen. Kept apart from FilmReel.cpp so that
// what fills a reel (DatasetPrep, and its tests) links without ImGui or GL.

#include "app/gui/FilmReel.h"

#include "app/gui/FeatureOverlay.h"
#include "app/gui/Layout.h"
#include "app/gui/Ui.h"
#include "i18n/catalog/Dataset.h"

#include <algorithm>

namespace dmsg = spirula::i18n::msg::dataset;

namespace gui {

bool FilmReel::draw(float height) {
    std::unique_lock<std::mutex> lk(_mu);
    const int n = (int)_frames.size();
    if (!n) return false;

    // Following means the newest picture HELD, not the newest frame: pictures
    // come only as fast as the screen keeps up, and chasing the rest to disk
    // would read files the producer is still writing.
    int newest = -1;
    for (const Slot& s : _cache) newest = std::max(newest, s.index);
    if (_follow) _shown = newest >= 0 ? newest : n - 1;
    _shown = std::clamp(_shown, 0, n - 1);

    ImGui::BeginChild("##reel", ImVec2(0, height));

    ImGui::BeginDisabled(n <= 1);
    if (ui::ButtonRaw("<") && _shown > 0) {
        _shown--;
        _follow = false;
    }
    ImGui::SameLine();
    if (ui::ButtonRaw(">") && _shown < n - 1) {
        _shown++;
        _follow = _shown == n - 1;
    }
    ImGui::SameLine();
    // "12 / 240" is two numbers and a separator: the same in every language.
    char fmt[32];
    std::snprintf(fmt, sizeof fmt, "%%d / %d", n);
    int one_based = _shown + 1;
    ImGui::SetNextItemWidth(px(190.0f));
    if (ui::SliderIntRaw("##reelidx", &one_based, 1, n, fmt)) {
        _shown = std::clamp(one_based - 1, 0, n - 1);
        _follow = _shown == n - 1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ui::Checkbox(dmsg::reel_follow, &_follow) && _follow) _shown = n - 1;
    ui::help_on_hover(dmsg::reel_follow_help);

    // What the pane can show, rounded up so that dragging a splitter does not
    // reload the picture on every frame of the drag.
    const ImVec2 box = ImGui::GetContentRegionAvail();
    const int want_side =
        std::clamp((((int)std::max(box.x, box.y) + 255) / 256) * 256, 256, 4096);
    _target = want_side;

    Slot* slot = find(_shown);
    if (slot && !slot->pic.empty()) {
        slot->used = ++_clock;
        // Also when the same frame came back at another size, which is what a
        // resized pane asks the loader for below.
        if (_tex_index != slot->index || _tex_w != slot->pic.w ||
            _tex_h != slot->pic.h) {
            if (!_tex) glGenTextures(1, &_tex);
            glBindTexture(GL_TEXTURE_2D, _tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, slot->pic.w, slot->pic.h, 0,
                         GL_RGB, GL_UNSIGNED_BYTE, slot->pic.rgb.data());
            _tex_index = slot->index;
            _tex_w = slot->pic.w;
            _tex_h = slot->pic.h;
            _tex_pts = slot->pts;
        }
    }
    if (!slot || !slot->pic.fits(want_side)) {
        _want = _shown;
        start_loader();
        _cv.notify_all();
    }

    if (_tex && _tex_w > 0 && _tex_h > 0) {
        const std::string& name = _frames[(size_t)_tex_index].name;
        if (!name.empty()) {
            ImGui::SameLine();
            ui::TextDisabledRaw(
                elide_middle(name, ImGui::GetContentRegionAvail().x));
        }
        const float s = std::min(box.x / (float)_tex_w, box.y / (float)_tex_h);
        if (s > 0.0f) {
            const ImVec2 size(_tex_w * s, _tex_h * s);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (box.x - size.x) * 0.5f);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::Image((ImTextureID)(intptr_t)_tex, size);
            // Features drawn over the picture rather than baked into it: the
            // same picture is shown before extraction reaches it.
            draw_keypoints(ImGui::GetWindowDrawList(), _tex_pts, at, size,
                           IM_COL32(120, 255, 140, 220));
        }
    }

    ImGui::EndChild();
    return true;
}

void FilmReel::destroy_gl() {
    std::lock_guard<std::mutex> lk(_mu);
    if (_tex) glDeleteTextures(1, &_tex);
    _tex = 0;
    _tex_index = -1;
    _tex_w = _tex_h = 0;
    _tex_pts.clear();
}

}  // namespace gui
