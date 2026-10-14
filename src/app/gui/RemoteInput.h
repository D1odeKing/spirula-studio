#pragma once

// What a remote session's browser sends: its mouse, keyboard, clipboard and
// size, as one JSON array per request (`/remote/input?e=[...]`). Parsing only;
// RemoteDisplay.cpp applies the events to ImGui. docs/notes/remote-gui.md.

#include <string>
#include <vector>

namespace gui {
namespace remote {

struct InputEvent {
    enum class Kind { Move, Button, Wheel, Key, Text, Resize, Clipboard, Focus };
    Kind kind = Kind::Move;
    float x = 0, y = 0;    // Move: window pixels. Wheel: steps, positive = up/left
    int button = 0;        // ImGuiMouseButton
    bool down = false;     // Button, Key; Focus: gained
    int key = 0;           // ImGuiKey, 0 for a code this build does not map
    bool ctrl = false, shift = false, alt = false, super = false;
    int w = 0, h = 0;      // Resize, in window pixels
    std::string text;      // Text, Clipboard
};

// The browser's KeyboardEvent.code ("KeyA", "ArrowLeft", "NumpadEnter") as an
// ImGuiKey, or 0. Layout-independent on purpose: shortcuts follow the physical
// key, and typed characters arrive separately as Text.
int imgui_key_from_code(const std::string& code);

// False with `error` set on malformed input; events before the bad one are kept.
bool parse_events(const std::string& json, std::vector<InputEvent>& out,
                  std::string& error);

}  // namespace remote
}  // namespace gui
