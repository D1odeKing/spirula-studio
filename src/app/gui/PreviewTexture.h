#pragma once

// One RGB picture as a GL texture, for the panels that draw pictures a worker
// thread made. GUI thread only, with the context current.

#include "app/gui/GlLoader.h"
#include "app/gui/Picture.h"

#include "imgui.h"

#include <algorithm>
#include <cstdint>

namespace gui {

struct PreviewTexture {
    GLuint tex = 0;
    int w = 0, h = 0;

    bool ready() const { return tex != 0 && w > 0 && h > 0; }
    ImTextureID id() const { return (ImTextureID)(intptr_t)tex; }

    void upload(const uint8_t* rgb, int width, int height) {
        if (!rgb || width <= 0 || height <= 0) return;
        if (!tex) glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        // A playing video replaces the picture every frame at the same size.
        if (width == w && height == h)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGB,
                            GL_UNSIGNED_BYTE, rgb);
        else
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB,
                         GL_UNSIGNED_BYTE, rgb);
        w = width;
        h = height;
    }
    void upload(const Picture& p) { upload(p.rgb.data(), p.w, p.h); }

    void reset() {
        if (tex) glDeleteTextures(1, &tex);
        tex = 0;
        w = h = 0;
    }
};

// The largest w:h box inside `box`, centred in it: where a picture of that
// shape goes in a pane.
inline void fit_rect(float w, float h, const ImVec2& at, const ImVec2& box,
                     ImVec2& min, ImVec2& max) {
    const float s = w > 0 && h > 0 ? std::min(box.x / w, box.y / h) : 0.0f;
    const ImVec2 size(w * s, h * s);
    min = ImVec2(at.x + (box.x - size.x) * 0.5f, at.y + (box.y - size.y) * 0.5f);
    max = ImVec2(min.x + size.x, min.y + size.y);
}

}  // namespace gui
