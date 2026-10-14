// RemoteInput.cpp -- see RemoteInput.h.

#include "app/gui/RemoteInput.h"

#include "data/Json.h"

#include "imgui.h"

#include <cstdlib>
#include <cstring>
#include <exception>

namespace gui {
namespace remote {

namespace {

struct CodeKey { const char* code; ImGuiKey key; };

const CodeKey kCodes[] = {
    {"Tab", ImGuiKey_Tab}, {"ArrowLeft", ImGuiKey_LeftArrow},
    {"ArrowRight", ImGuiKey_RightArrow}, {"ArrowUp", ImGuiKey_UpArrow},
    {"ArrowDown", ImGuiKey_DownArrow}, {"PageUp", ImGuiKey_PageUp},
    {"PageDown", ImGuiKey_PageDown}, {"Home", ImGuiKey_Home}, {"End", ImGuiKey_End},
    {"Insert", ImGuiKey_Insert}, {"Delete", ImGuiKey_Delete},
    {"Backspace", ImGuiKey_Backspace}, {"Space", ImGuiKey_Space},
    {"Enter", ImGuiKey_Enter}, {"Escape", ImGuiKey_Escape},
    {"ControlLeft", ImGuiKey_LeftCtrl}, {"ControlRight", ImGuiKey_RightCtrl},
    {"ShiftLeft", ImGuiKey_LeftShift}, {"ShiftRight", ImGuiKey_RightShift},
    {"AltLeft", ImGuiKey_LeftAlt}, {"AltRight", ImGuiKey_RightAlt},
    {"MetaLeft", ImGuiKey_LeftSuper}, {"MetaRight", ImGuiKey_RightSuper},
    {"ContextMenu", ImGuiKey_Menu}, {"Quote", ImGuiKey_Apostrophe},
    {"Comma", ImGuiKey_Comma}, {"Minus", ImGuiKey_Minus}, {"Period", ImGuiKey_Period},
    {"Slash", ImGuiKey_Slash}, {"Semicolon", ImGuiKey_Semicolon},
    {"Equal", ImGuiKey_Equal}, {"BracketLeft", ImGuiKey_LeftBracket},
    {"Backslash", ImGuiKey_Backslash}, {"BracketRight", ImGuiKey_RightBracket},
    {"Backquote", ImGuiKey_GraveAccent}, {"CapsLock", ImGuiKey_CapsLock},
    {"ScrollLock", ImGuiKey_ScrollLock}, {"NumLock", ImGuiKey_NumLock},
    {"PrintScreen", ImGuiKey_PrintScreen}, {"Pause", ImGuiKey_Pause},
    {"NumpadDecimal", ImGuiKey_KeypadDecimal}, {"NumpadDivide", ImGuiKey_KeypadDivide},
    {"NumpadMultiply", ImGuiKey_KeypadMultiply}, {"NumpadSubtract", ImGuiKey_KeypadSubtract},
    {"NumpadAdd", ImGuiKey_KeypadAdd}, {"NumpadEnter", ImGuiKey_KeypadEnter},
    {"NumpadEqual", ImGuiKey_KeypadEqual},
};

bool flag(const JsonValue& e, const char* key) {
    const JsonValue* v = e.find(key);
    return v && (v->type == JsonValue::Type::Bool ? v->b : v->as_double(0) != 0);
}

}  // namespace

int imgui_key_from_code(const std::string& code) {
    for (const CodeKey& c : kCodes)
        if (code == c.code) return (int)c.key;
    const size_t n = code.size();
    if (n == 4 && code.compare(0, 3, "Key") == 0 && code[3] >= 'A' && code[3] <= 'Z')
        return (int)(ImGuiKey_A + (code[3] - 'A'));
    if (n == 6 && code.compare(0, 5, "Digit") == 0 && code[5] >= '0' && code[5] <= '9')
        return (int)(ImGuiKey_0 + (code[5] - '0'));
    if (n == 7 && code.compare(0, 6, "Numpad") == 0 && code[6] >= '0' && code[6] <= '9')
        return (int)(ImGuiKey_Keypad0 + (code[6] - '0'));
    if (n >= 2 && n <= 3 && code[0] == 'F') {
        const int f = std::atoi(code.c_str() + 1);
        if (f >= 1 && f <= 24) return (int)(ImGuiKey_F1 + (f - 1));
    }
    return 0;
}

bool parse_events(const std::string& json, std::vector<InputEvent>& out,
                  std::string& error) {
    JsonValue root;
    try {
        root = json_parse(json);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    if (!root.is_array()) {
        error = "expected a JSON array of events";
        return false;
    }
    for (const JsonValue& e : root.arr) {
        const std::string& t = e.find("t") ? e.find("t")->as_string() : std::string();
        InputEvent ev;
        ev.ctrl = flag(e, "c");
        ev.shift = flag(e, "s");
        ev.alt = flag(e, "a");
        ev.super = flag(e, "m");
        if (t == "m") {
            ev.kind = InputEvent::Kind::Move;
            ev.x = (float)e.get_double("x", 0);
            ev.y = (float)e.get_double("y", 0);
        } else if (t == "b") {
            ev.kind = InputEvent::Kind::Button;
            ev.button = (int)e.get_double("b", 0);
            ev.down = flag(e, "d");
            if (ev.button < 0 || ev.button >= ImGuiMouseButton_COUNT) {
                error = "mouse button out of range";
                return false;
            }
        } else if (t == "w") {
            ev.kind = InputEvent::Kind::Wheel;
            ev.x = (float)e.get_double("x", 0);
            ev.y = (float)e.get_double("y", 0);
        } else if (t == "k") {
            ev.kind = InputEvent::Kind::Key;
            ev.key = imgui_key_from_code(e.find("code") ? e.find("code")->as_string() : "");
            ev.down = flag(e, "d");
        } else if (t == "t" || t == "clip") {
            ev.kind = t == "t" ? InputEvent::Kind::Text : InputEvent::Kind::Clipboard;
            ev.text = e.find("v") ? e.find("v")->as_string() : std::string();
        } else if (t == "size") {
            ev.kind = InputEvent::Kind::Resize;
            ev.w = (int)e.get_double("w", 0);
            ev.h = (int)e.get_double("h", 0);
            if (ev.w < 320 || ev.h < 240 || ev.w > 7680 || ev.h > 4320) {
                error = "window size out of range";
                return false;
            }
        } else if (t == "f") {
            ev.kind = InputEvent::Kind::Focus;
            ev.down = flag(e, "d");
        } else {
            error = "unknown event type '" + t + "'";
            return false;
        }
        out.push_back(std::move(ev));
    }
    return true;
}

}  // namespace remote
}  // namespace gui
