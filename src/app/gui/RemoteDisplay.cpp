// RemoteDisplay.cpp -- see RemoteDisplay.h.

#include "app/gui/RemoteDisplay.h"

#include "app/gui/GlLoader.h"
#include "app/gui/RemoteInput.h"
#include "app/webviewer/HttpServer.h"
#include "app_generated/remote_gui_html.h"   // kRemoteGuiHtml[] (CMake-embedded)
#include "core/Env.h"
#include "external/stb_image_write.h"
#include "i18n/catalog/Log.h"

#include "imgui.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace gui {
namespace remote {

namespace {

// The Xvfb screen: the largest window a page may ask for. Its framebuffer is
// host memory, 59 MB at this size, whatever the window uses of it.
constexpr int kScreenW = 5120, kScreenH = 2880;

struct Url { uint64_t n; std::string url; };

struct State {
    bool active = false;
    Options opt;
    HttpServer http;
    GLFWwindow* window = nullptr;

    std::mutex mu;
    std::condition_variable frame_cv;   // a new JPEG, or shutting down
    std::condition_variable encode_cv;  // a new frame to encode, or shutting down
    bool stopping = false;

    std::vector<InputEvent> input;      // page -> GUI thread
    std::string clip_in;                // the page's clipboard, for Ctrl+V
    std::string clip_out;               // what the GUI copied, for the page
    uint64_t clip_seq = 0;
    std::deque<Url> urls;
    uint64_t url_seq = 0;
    int cursor = ImGuiMouseCursor_Arrow;

    std::vector<uint8_t> rgba;          // the frame waiting to be encoded
    int rgba_w = 0, rgba_h = 0;
    bool rgba_pending = false;
    std::vector<uint8_t> jpeg;          // the newest encoded frame
    uint64_t jpeg_seq = 0;
    int jpeg_w = 0, jpeg_h = 0;
    int viewers = 0;
    bool force = false;                 // send the next frame even if unchanged
    std::thread encoder;

    // GUI thread only.
    uint64_t last_hash = 0;
    int last_fb_w = 0, last_fb_h = 0;
    double last_read = 0.0;
    std::vector<uint8_t> readback;
};

State& st() {
    static State s;
    return s;
}

uint64_t fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 1099511628211ull;
    }
    return h;
}

void jpeg_write(void* ctx, void* data, int size) {
    auto* out = (std::vector<uint8_t>*)ctx;
    out->insert(out->end(), (uint8_t*)data, (uint8_t*)data + size);
}

// GL reads bottom-up and with alpha; the page wants top-down RGB.
void encode_loop() {
    State& s = st();
    std::vector<uint8_t> rgba, rgb, jpg;
    for (;;) {
        int w = 0, h = 0, quality = 80;
        {
            std::unique_lock<std::mutex> lk(s.mu);
            s.encode_cv.wait(lk, [&] { return s.stopping || s.rgba_pending; });
            if (s.stopping) return;
            rgba.swap(s.rgba);
            w = s.rgba_w;
            h = s.rgba_h;
            quality = s.opt.quality;
            s.rgba_pending = false;
        }
        rgb.resize((size_t)w * h * 3);
        for (int y = 0; y < h; y++) {
            const uint8_t* src = rgba.data() + (size_t)(h - 1 - y) * w * 4;
            uint8_t* dst = rgb.data() + (size_t)y * w * 3;
            for (int x = 0; x < w; x++) {
                dst[x * 3 + 0] = src[x * 4 + 0];
                dst[x * 3 + 1] = src[x * 4 + 1];
                dst[x * 3 + 2] = src[x * 4 + 2];
            }
        }
        jpg.clear();
        stbi_write_jpg_to_func(jpeg_write, &jpg, w, h, 3, rgb.data(), quality);
        {
            std::lock_guard<std::mutex> lk(s.mu);
            s.jpeg.swap(jpg);
            s.jpeg_w = w;
            s.jpeg_h = h;
            s.jpeg_seq++;
        }
        s.frame_cv.notify_all();
    }
}

const char* cursor_name(int c) {
    switch (c) {
        case ImGuiMouseCursor_None: return "none";
        case ImGuiMouseCursor_TextInput: return "text";
        case ImGuiMouseCursor_ResizeAll: return "move";
        case ImGuiMouseCursor_ResizeNS: return "ns-resize";
        case ImGuiMouseCursor_ResizeEW: return "ew-resize";
        case ImGuiMouseCursor_ResizeNESW: return "nesw-resize";
        case ImGuiMouseCursor_ResizeNWSE: return "nwse-resize";
        case ImGuiMouseCursor_Hand: return "pointer";
        case ImGuiMouseCursor_Wait: return "wait";
        case ImGuiMouseCursor_Progress: return "progress";
        case ImGuiMouseCursor_NotAllowed: return "not-allowed";
        default: return "default";
    }
}

void json_str(std::string& out, const std::string& v) {
    out += '"';
    for (unsigned char c : v) {
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else out += (char)c;
    }
    out += '"';
}

HttpResponse handle_page(const HttpRequest&) {
    HttpResponse r;
    r.content_type = "text/html; charset=utf-8";
    r.body.assign(kRemoteGuiHtml, kRemoteGuiHtml + kRemoteGuiHtmlSize);
    return r;
}

HttpResponse handle_input(const HttpRequest& req) {
    std::vector<InputEvent> events;
    std::string error;
    const bool ok = parse_events(req.get("e", "[]"), events, error);
    State& s = st();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        for (InputEvent& e : events) s.input.push_back(std::move(e));
    }
    // The frame loop sleeps between frames when idle; this is what wakes it.
    glfwPostEmptyEvent();
    if (!ok) return HttpResponse::text(400, error);
    return HttpResponse::json("{\"ok\":true}");
}

HttpResponse handle_status(const HttpRequest& req) {
    State& s = st();
    const uint64_t urls_seen = (uint64_t)std::max(0, req.get_int("urls", 0));
    std::string out = "{";
    std::lock_guard<std::mutex> lk(s.mu);
    out += "\"frame\":" + std::to_string(s.jpeg_seq);
    out += ",\"w\":" + std::to_string(s.jpeg_w) + ",\"h\":" + std::to_string(s.jpeg_h);
    out += ",\"cursor\":\"" + std::string(cursor_name(s.cursor)) + "\"";
    out += ",\"clip\":" + std::to_string(s.clip_seq);
    out += ",\"urls\":[";
    bool first = true;
    for (const Url& u : s.urls) {
        if (u.n <= urls_seen) continue;
        out += first ? "" : ",";
        first = false;
        out += "{\"n\":" + std::to_string(u.n) + ",\"url\":";
        json_str(out, u.url);
        out += "}";
    }
    out += "]}";
    return HttpResponse::json(out);
}

HttpResponse handle_clipboard(const HttpRequest&) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    return HttpResponse::text(200, s.clip_out, "text/plain; charset=utf-8");
}

// MJPEG: one trip across the tunnel per frame. Chrome shows a part only once
// the next begins, so the newest is re-sent if nothing follows in kSettleMs.
void handle_stream(const HttpRequest&, HttpStream& stream) {
    State& s = st();
    if (!stream.begin("multipart/x-mixed-replace; boundary=spirulaframe") ||
        !stream.send("--spirulaframe\r\n"))
        return;
    constexpr int kSettleMs = 150;
    uint64_t sent = 0;
    bool settled = true;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.viewers++;
        s.force = true;
    }
    glfwPostEmptyEvent();
    std::vector<uint8_t> frame;
    while (stream.alive()) {
        {
            std::unique_lock<std::mutex> lk(s.mu);
            s.frame_cv.wait_for(lk, std::chrono::milliseconds(settled ? 500 : kSettleMs),
                                [&] { return s.stopping || s.jpeg_seq != sent; });
            if (s.stopping) break;
            if (s.jpeg_seq == sent) {
                if (settled) continue;
                settled = true;   // resend `frame`, the newest
            } else {
                frame = s.jpeg;
                sent = s.jpeg_seq;
                settled = false;
            }
        }
        const std::string head = "Content-Type: image/jpeg\r\nContent-Length: " +
                                 std::to_string(frame.size()) + "\r\n\r\n";
        if (!stream.send(head) || !stream.send(frame.data(), frame.size()) ||
            !stream.send("\r\n--spirulaframe\r\n"))
            break;
    }
    std::lock_guard<std::mutex> lk(s.mu);
    s.viewers--;
}

const char* get_clipboard(ImGuiContext*) {
    State& s = st();
    static std::string held;   // ImGui keeps the pointer until the next call
    std::lock_guard<std::mutex> lk(s.mu);
    held = s.clip_in;
    return held.c_str();
}

void set_clipboard(ImGuiContext*, const char* text) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    s.clip_out = text ? text : "";
    s.clip_in = s.clip_out;
    s.clip_seq++;
}

void apply(const InputEvent& e, GLFWwindow* window, float scale) {
    ImGuiIO& io = ImGui::GetIO();
    switch (e.kind) {
        case InputEvent::Kind::Move:
            io.AddMousePosEvent(e.x / scale, e.y / scale);
            break;
        case InputEvent::Kind::Button:
            io.AddMouseButtonEvent(e.button, e.down);
            break;
        case InputEvent::Kind::Wheel:
            io.AddMouseWheelEvent(e.x, e.y);
            break;
        case InputEvent::Kind::Key:
            io.AddKeyEvent(ImGuiMod_Ctrl, e.ctrl);
            io.AddKeyEvent(ImGuiMod_Shift, e.shift);
            io.AddKeyEvent(ImGuiMod_Alt, e.alt);
            io.AddKeyEvent(ImGuiMod_Super, e.super);
            if (e.key) io.AddKeyEvent((ImGuiKey)e.key, e.down);
            break;
        case InputEvent::Kind::Text:
            io.AddInputCharactersUTF8(e.text.c_str());
            break;
        case InputEvent::Kind::Clipboard: {
            State& s = st();
            std::lock_guard<std::mutex> lk(s.mu);
            s.clip_in = e.text;
            break;
        }
        case InputEvent::Kind::Resize: {
            const int w = std::min(e.w, kScreenW), h = std::min(e.h, kScreenH);
            int cw = 0, ch = 0;
            glfwGetWindowSize(window, &cw, &ch);
            if (cw != w || ch != h) glfwSetWindowSize(window, w, h);
            break;
        }
        case InputEvent::Kind::Focus:
            io.AddFocusEvent(e.down);
            break;
    }
}

#if defined(__linux__)
pid_t g_xvfb = -1;

void stop_xvfb() {
    if (g_xvfb > 0) {
        kill(g_xvfb, SIGTERM);
        waitpid(g_xvfb, nullptr, 0);
        g_xvfb = -1;
    }
}
#endif

}  // namespace

bool take_args(int& argc, char** argv, Options& out, std::string& error) {
    if (const char* p = spirula::env("GUI_REMOTE")) out.port = std::atoi(p);
    std::vector<char*> keep;
    keep.push_back(argv[0]);
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        std::string key = a, value;
        bool has_value = false;
        if (const size_t eq = a.find('='); a.rfind("--remote", 0) == 0 && eq != std::string::npos) {
            key = a.substr(0, eq);
            value = a.substr(eq + 1);
            has_value = true;
        }
        if (key != "--remote" && key != "--remote-host" && key != "--remote-token" &&
            key != "--remote-quality" && key != "--remote-fps") {
            keep.push_back(argv[i]);
            continue;
        }
        if (!has_value) {
            if (i + 1 >= argc) {
                error = key + ": missing value";
                return false;
            }
            value = argv[++i];
        }
        auto number = [&](int lo, int hi, int& dst) {
            char* end = nullptr;
            const long v = std::strtol(value.c_str(), &end, 10);
            if (end == value.c_str() || *end || v < lo || v > hi) {
                error = key + ": expected a number from " + std::to_string(lo) + " to " +
                        std::to_string(hi) + ", got '" + value + "'";
                return false;
            }
            dst = (int)v;
            return true;
        };
        if (key == "--remote" && !number(1, 65535, out.port)) return false;
        if (key == "--remote-quality" && !number(1, 100, out.quality)) return false;
        if (key == "--remote-fps" && !number(1, 60, out.max_fps)) return false;
        if (key == "--remote-host") out.host = value;
        if (key == "--remote-token") out.token = value;
    }
    if (out.port < 0 || out.port > 65535) {
        error = "SS_GUI_REMOTE: not a port";
        return false;
    }
    for (size_t i = 0; i < keep.size(); i++) argv[i] = keep[i];
    argc = (int)keep.size();
    argv[argc] = nullptr;
    return true;
}

bool ensure_display(std::string& error) {
#if defined(__linux__)
    if (const char* d = std::getenv("DISPLAY"); d && *d) return true;
    namespace fs = std::filesystem;
    int n = 99;
    for (; n < 200; n++) {
        const std::string lock = "/tmp/.X" + std::to_string(n) + "-lock";
        const std::string sock = "/tmp/.X11-unix/X" + std::to_string(n);
        std::error_code ec;
        if (!fs::exists(lock, ec) && !fs::exists(sock, ec)) break;
    }
    const std::string display = ":" + std::to_string(n);
    const std::string screen = std::to_string(kScreenW) + "x" + std::to_string(kScreenH) + "x24";
    const pid_t pid = fork();
    if (pid < 0) {
        error = "cannot start Xvfb: fork failed";
        return false;
    }
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);   // never outlive the GUI
        // xkbcomp's keysym warnings on every start are not the GUI's log.
        if (const int null_fd = open("/dev/null", O_WRONLY); null_fd >= 0) {
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
        }
        execlp("Xvfb", "Xvfb", display.c_str(), "-screen", "0", screen.c_str(),
               "-nolisten", "tcp", "-noreset", (char*)nullptr);
        _exit(127);
    }
    g_xvfb = pid;
    std::atexit(stop_xvfb);
    const std::string sock = "/tmp/.X11-unix/X" + std::to_string(n);
    for (int i = 0; i < 100; i++) {
        std::error_code ec;
        if (fs::exists(sock, ec)) {
            setenv("DISPLAY", display.c_str(), 1);
            return true;
        }
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            g_xvfb = -1;
            error = WIFEXITED(status) && WEXITSTATUS(status) == 127
                        ? "no display, and Xvfb is not installed (apt install xvfb)"
                        : "Xvfb exited before it was ready";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    error = "Xvfb did not start within 5 s";
    return false;
#else
    (void)error;
    return true;
#endif
}

void start(const Options& opt, GLFWwindow* window) {
    State& s = st();
    s.opt = opt;
    s.window = window;
    if (s.opt.token.empty()) s.opt.token = HttpServer::make_token();

    s.http.set_token(s.opt.token);
    s.http.route("/", handle_page);
    s.http.route("/remote/input", handle_input);
    s.http.route("/remote/status", handle_status);
    s.http.route("/remote/clipboard", handle_clipboard);
    s.http.route_stream("/remote/stream", handle_stream);
    s.http.start(s.opt.host, s.opt.port);

    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    pio.Platform_GetClipboardTextFn = get_clipboard;
    pio.Platform_SetClipboardTextFn = set_clipboard;
    // The hidden window is never focused, and ImGui drops held input on focus loss.
    ImGui::GetIO().ConfigDebugIgnoreFocusLoss = true;

    s.stopping = false;
    s.encoder = std::thread(encode_loop);
    s.active = true;

    const std::string shown = s.opt.host == "0.0.0.0" ? "127.0.0.1" : s.opt.host;
    std::printf("%s\n", spirula::i18n::format(spirula::i18n::msg::log::remote_gui_at,
                                              {shown, s.opt.port, s.opt.token})
                            .c_str());
    std::printf("%s\n", spirula::i18n::format(spirula::i18n::msg::log::remote_gui_tunnel,
                                              {s.opt.port})
                            .c_str());
    std::fflush(stdout);
}

bool active() { return st().active; }

bool watched() {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.active && s.viewers > 0;
}

void begin_frame(GLFWwindow* window) {
    State& s = st();
    if (!s.active) return;
    std::vector<InputEvent> events;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        events.swap(s.input);
    }
    int win_w = 0, fb_w = 0, h = 0;
    glfwGetWindowSize(window, &win_w, &h);
    glfwGetFramebufferSize(window, &fb_w, &h);
    const float scale = win_w > 0 && fb_w > 0 ? (float)fb_w / (float)win_w : 1.0f;
    for (const InputEvent& e : events) apply(e, window, scale);
}

void end_frame(int fb_w, int fb_h) {
    State& s = st();
    if (!s.active || fb_w <= 0 || fb_h <= 0) return;
    bool force = false;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.cursor = ImGui::GetMouseCursor();
        if (s.viewers == 0) return;
        force = s.force;
    }
    // The frame after a resize can still be drawn into the old-sized buffer
    // (Mesa on Xvfb), which reads back shifted; the next one is right.
    if (fb_w != s.last_fb_w || fb_h != s.last_fb_h) {
        s.last_fb_w = fb_w;
        s.last_fb_h = fb_h;
        std::lock_guard<std::mutex> lk(s.mu);
        s.force = true;
        return;
    }
    const double now = glfwGetTime();
    if (!force && now - s.last_read < 1.0 / s.opt.max_fps) return;

    s.readback.resize((size_t)fb_w * fb_h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fb_w, fb_h, GL_RGBA, GL_UNSIGNED_BYTE, s.readback.data());
    s.last_read = now;
    const uint64_t hash = fnv1a(s.readback.data(), s.readback.size()) ^ ((uint64_t)fb_w << 32 | (uint64_t)fb_h);
    if (!force && hash == s.last_hash) return;
    s.last_hash = hash;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.force = false;
        s.rgba.swap(s.readback);
        s.rgba_w = fb_w;
        s.rgba_h = fb_h;
        s.rgba_pending = true;
    }
    s.encode_cv.notify_one();
}

bool open_url(const std::string& url) {
    State& s = st();
    if (!s.active) return false;
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return false;
    std::lock_guard<std::mutex> lk(s.mu);
    s.urls.push_back({++s.url_seq, url});
    while (s.urls.size() > 16) s.urls.pop_front();
    return true;
}

void shutdown() {
    State& s = st();
    if (!s.active) return;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.stopping = true;
    }
    s.frame_cv.notify_all();
    s.encode_cv.notify_all();
    s.http.stop();
    if (s.encoder.joinable()) s.encoder.join();
    s.active = false;
}

}  // namespace remote
}  // namespace gui
