#pragma once

// The whole GUI in a browser on another machine: `spirula gui --remote PORT`.
//
// The window stays hidden on the machine it runs on (a cloud GPU), each frame
// that changed is JPEG-encoded and pushed to the page as an MJPEG stream, and
// the page sends its mouse, keyboard, clipboard and size back. Everything the
// GUI opens or saves is on that machine. Same loopback + token model as the
// SfM monitor. Design and use: docs/notes/remote-gui.md.

#include <cstdint>
#include <string>

struct GLFWwindow;

namespace gui {
namespace remote {

struct Options {
    int port = 0;                      // 0: not a remote session
    std::string host = "127.0.0.1";
    std::string token;                 // empty: a random one
    int quality = 80;                  // JPEG, 1..100
    int max_fps = 30;
};

// Takes --remote PORT, --remote-host HOST, --remote-token T, --remote-quality Q
// and --remote-fps N out of argv; SS_GUI_REMOTE=PORT arms it too. False with
// `error` set on a bad value.
bool take_args(int& argc, char** argv, Options& out, std::string& error);

// Linux with no DISPLAY: starts an Xvfb for the hidden window and points
// DISPLAY at it. True when there is a display to use afterwards.
bool ensure_display(std::string& error);

// Binds the server and prints the address. Call after the ImGui backends are
// initialised (it replaces their clipboard). Throws on a bind failure.
void start(const Options& opt, GLFWwindow* window);
bool active();
// A page is connected; with none, the frame loop can idle as if iconified.
bool watched();

// Between ImGui_ImplGlfw_NewFrame() and ImGui::NewFrame(): applies the input
// and the size the page sent since the last frame.
void begin_frame(GLFWwindow* window);

// After the draw data has been rendered, before the swap: reads the frame back
// when a page is watching and it changed.
void end_frame(int fb_w, int fb_h);

// A link the GUI wanted opened, shown to the page instead; false for anything
// but http(s), which the page could not open anyway.
bool open_url(const std::string& url);

// Files, on this machine, handed to the page (docs/notes/file-browser.md):
// a splat or cloud for its web viewer, a file or folder to download, a folder
// to upload into, a video for the browser to play.
void open_in_viewer(const std::string& path);
void download(const std::string& path);
void request_upload(const std::string& dir);
void play_video(const std::string& path);
// Where a file dropped on the page goes; "" refuses drops.
void set_drop_dir(const std::string& dir);
// Uploads finished so far, so a listing knows to refresh.
uint64_t upload_count();

void shutdown();

}  // namespace remote
}  // namespace gui
