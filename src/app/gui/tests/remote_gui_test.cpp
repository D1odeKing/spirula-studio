// remote_gui -- what `spirula gui --remote` depends on that needs no window:
// the page's input events, and HttpServer's stream routes over a real socket
// (a stream must not hold up the ordinary routes the input arrives on).

#include "app/gui/RemoteInput.h"
#include "app/webviewer/HttpServer.h"

#include "imgui.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET socket_t;
  static void close_socket(socket_t s) { closesocket(s); }
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  typedef int socket_t;
  static void close_socket(socket_t s) { ::close(s); }
#endif

using gui::remote::InputEvent;

namespace {

int g_failures = 0;

void expect(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "ok  " : "BAD ", what.c_str());
    if (!ok) g_failures++;
}

socket_t connect_to(int port) {
    socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, (sockaddr*)&addr, sizeof addr) != 0) {
        close_socket(fd);
        return (socket_t)-1;
    }
    return fd;
}

// Reads until `want` appears or the peer closes; what arrived either way.
std::string read_until(socket_t fd, const std::string& want) {
    std::string got;
    char buf[4096];
    while (got.find(want) == std::string::npos) {
        const int n = (int)recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        got.append(buf, (size_t)n);
    }
    return got;
}

std::string get(int port, const std::string& target) {
    socket_t fd = connect_to(port);
    if (fd == (socket_t)-1) return "";
    const std::string req = "GET " + target + " HTTP/1.0\r\n\r\n";
    send(fd, req.data(), (int)req.size(), 0);
    std::string got = read_until(fd, "\x01never");
    close_socket(fd);
    return got;
}

void test_keys() {
    using gui::remote::imgui_key_from_code;
    expect(imgui_key_from_code("KeyA") == ImGuiKey_A, "KeyA -> A");
    expect(imgui_key_from_code("KeyZ") == ImGuiKey_Z, "KeyZ -> Z");
    expect(imgui_key_from_code("Digit7") == ImGuiKey_7, "Digit7 -> 7");
    expect(imgui_key_from_code("Numpad3") == ImGuiKey_Keypad3, "Numpad3 -> Keypad3");
    expect(imgui_key_from_code("F12") == ImGuiKey_F12, "F12");
    expect(imgui_key_from_code("ArrowLeft") == ImGuiKey_LeftArrow, "ArrowLeft");
    expect(imgui_key_from_code("ControlRight") == ImGuiKey_RightCtrl, "ControlRight");
    expect(imgui_key_from_code("NumpadEnter") == ImGuiKey_KeypadEnter, "NumpadEnter");
    expect(imgui_key_from_code("Keya") == 0 && imgui_key_from_code("F25") == 0 &&
               imgui_key_from_code("") == 0,
           "unknown codes map to 0");
}

void test_parse() {
    std::vector<InputEvent> ev;
    std::string err;
    const bool ok = gui::remote::parse_events(
        R"([{"t":"m","x":10.5,"y":20},{"t":"b","b":1,"d":1},{"t":"w","x":0,"y":-1.5},)"
        R"({"t":"k","code":"KeyS","d":1,"c":1,"s":0},{"t":"t","v":"éa"},)"
        R"({"t":"clip","v":"/workspace/data"},{"t":"size","w":1600,"h":900},{"t":"f","d":0}])",
        ev, err);
    expect(ok && ev.size() == 8, "a batch of every kind parses");
    if (ev.size() == 8) {
        expect(ev[0].kind == InputEvent::Kind::Move && ev[0].x == 10.5f && ev[0].y == 20,
               "move keeps its position");
        expect(ev[1].kind == InputEvent::Kind::Button && ev[1].button == 1 && ev[1].down,
               "right button down");
        expect(ev[2].kind == InputEvent::Kind::Wheel && ev[2].y == -1.5f, "wheel");
        expect(ev[3].kind == InputEvent::Kind::Key && ev[3].key == ImGuiKey_S && ev[3].ctrl &&
                   !ev[3].shift,
               "Ctrl+S carries its modifiers");
        expect(ev[4].kind == InputEvent::Kind::Text && ev[4].text == "\xc3\xa9" "a",
               "text arrives as UTF-8");
        expect(ev[5].kind == InputEvent::Kind::Clipboard && ev[5].text == "/workspace/data",
               "clipboard");
        expect(ev[6].kind == InputEvent::Kind::Resize && ev[6].w == 1600 && ev[6].h == 900,
               "resize");
        expect(ev[7].kind == InputEvent::Kind::Focus && !ev[7].down, "focus lost");
    }
    ev.clear();
    expect(!gui::remote::parse_events(R"([{"t":"size","w":10,"h":10}])", ev, err),
           "a window too small is refused");
    expect(!gui::remote::parse_events(R"([{"t":"b","b":9,"d":1}])", ev, err),
           "a button ImGui does not have is refused");
    expect(!gui::remote::parse_events(R"([{"t":"zz"}])", ev, err), "unknown type refused");
    expect(!gui::remote::parse_events(R"({"t":"m"})", ev, err), "not an array refused");
    expect(!gui::remote::parse_events("[{", ev, err), "malformed JSON refused");
}

void test_stream() {
    HttpServer http;
    http.set_token("secret");
    std::atomic<int> streams{0};
    http.route("/ping", [](const HttpRequest&) { return HttpResponse::text(200, "pong"); });
    http.route_stream("/stream", [&](const HttpRequest&, HttpStream& s) {
        streams++;
        if (!s.begin("multipart/x-mixed-replace; boundary=b")) return;
        for (int i = 0; i < 1000 && s.alive(); i++) {
            const std::string part = "--b\r\nContent-Type: text/plain\r\n\r\nframe" +
                                     std::to_string(i) + "\r\n";
            if (!s.send(part)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    int port = 0;
    for (int p = 18731; p < 18800 && !port; p++) {
        try {
            http.start("127.0.0.1", p);
            port = p;
        } catch (...) {}
    }
    expect(port != 0, "server binds");
    if (!port) return;

    expect(get(port, "/stream").find(" 403 ") != std::string::npos,
           "a stream without the token is refused");

    socket_t fd = connect_to(port);
    const std::string req = "GET /stream?token=secret HTTP/1.0\r\n\r\n";
    send(fd, req.data(), (int)req.size(), 0);
    const std::string head = read_until(fd, "frame2");
    expect(head.find("HTTP/1.0 200 OK") == 0, "the stream starts with 200");
    expect(head.find("multipart/x-mixed-replace") != std::string::npos,
           "and its own content type");
    expect(head.find("Access-Control-Allow-Origin") == std::string::npos,
           "no CORS header behind a token");
    expect(head.find("frame0") != std::string::npos && head.find("frame2") != std::string::npos,
           "parts keep arriving on one connection");

    // The serial loop is free while the stream runs.
    const auto t0 = std::chrono::steady_clock::now();
    const std::string pong = get(port, "/ping?token=secret");
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    expect(pong.find("pong") != std::string::npos && ms < 1000,
           "an ordinary route answers while a stream is open");

    std::vector<socket_t> extra;
    for (int i = 0; i < 16; i++) {
        socket_t f = connect_to(port);
        send(f, req.data(), (int)req.size(), 0);
        extra.push_back(f);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    socket_t last = extra.back();
    const std::string over = read_until(last, "\r\n\r\n");
    expect(over.find(" 503 ") != std::string::npos, "a seventeenth stream is refused");
    for (socket_t f : extra) close_socket(f);
    close_socket(fd);

    const auto s0 = std::chrono::steady_clock::now();
    http.stop();
    const double stop_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - s0).count();
    expect(stop_ms < 2000, "stop() ends the streams promptly");
    expect(streams.load() == 16, "sixteen streams ran");
}

}  // namespace

int main() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    test_keys();
    test_parse();
    test_stream();
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
