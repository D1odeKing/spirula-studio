#pragma once

// HttpServer -- minimal dependency-free HTTP/1.0 server for the web viewer,
// the SfM monitor and the GUI's automation surface.
//
// Deliberately tiny: GET only, serial request handling on one background
// thread (the viewer protocol is polling + latest-wins, so serialization is
// part of the contract), Connection: close per request. Handlers run on the
// server thread; anything slow (rendering) hands off and waits. A stream route
// is the exception: it gets a thread of its own, keeps its connection, and may
// be a POST whose body it reads (uploads).
//
// POSIX sockets on Linux/macOS; winsock on Windows (compiles, untested --
// cloud training boxes are Linux).

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <map>
#include <string>
#include <thread>
#include <vector>

struct HttpRequest {
    std::string method;                          // "GET"
    std::string path;                            // "/render" (percent-decoded)
    std::map<std::string, std::string> query;    // decoded key -> value
    std::map<std::string, std::string> headers;  // lower-case name -> value

    std::string header(const std::string& lower_name) const {
        auto it = headers.find(lower_name);
        return it == headers.end() ? std::string() : it->second;
    }
    std::string cookie(const std::string& name) const;

    std::string get(const std::string& key, const std::string& def = "") const {
        auto it = query.find(key);
        return it == query.end() ? def : it->second;
    }
    double get_double(const std::string& key, double def) const;
    int    get_int(const std::string& key, int def) const;
    bool   get_bool(const std::string& key, bool def) const;
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "text/plain";
    std::vector<uint8_t> body;
    std::vector<std::pair<std::string, std::string>> headers;   // extra ones

    static HttpResponse text(int status, const std::string& s,
                             const std::string& type = "text/plain") {
        HttpResponse r;
        r.status = status;
        r.content_type = type;
        r.body.assign(s.begin(), s.end());
        return r;
    }
    static HttpResponse json(const std::string& s) {
        return text(200, s, "application/json");
    }
};

// The connection a stream route writes to, for as long as it likes.
class HttpStream {
public:
    // The status line and headers; call once, before any send(). `extra` is
    // whole header lines, each ending in \r\n.
    bool begin(const std::string& content_type, int status = 200,
               const std::string& extra = "");
    // False once the client has gone or the server is stopping.
    bool send(const void* data, size_t len);
    bool send(const std::string& s) { return send(s.data(), s.size()); }
    bool alive() const { return _ok && _running->load(); }

    // A POST's body, Content-Length bytes of it: what fits in `cap` per call,
    // 0 once it is all read, -1 when it stopped short (or stop() was called,
    // or nothing came for a minute).
    int64_t read(void* buf, size_t cap);
    int64_t body_size() const { return _body_size; }

private:
    friend class HttpServer;
    intptr_t _fd = -1;
    bool _ok = true;
    bool _cors = false;
    const std::atomic<bool>* _running = nullptr;
    std::string _pending;          // body bytes that arrived with the head
    int64_t _body_size = 0, _body_read = 0;
};

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;
    // Runs on its own thread; returns when it is done or stream.alive() is false.
    using StreamHandler = std::function<void(const HttpRequest&, HttpStream&)>;

    // Exact-path routes; unmatched paths get a 404. A prefix route answers
    // every path that starts with `prefix`, longest prefix first.
    void route(const std::string& path, Handler h) { _routes[path] = std::move(h); }
    void route_prefix(const std::string& prefix, Handler h) {
        _prefix_routes[prefix] = std::move(h);
    }
    void route_stream(const std::string& path, StreamHandler h, bool prefix = false) {
        (prefix ? _prefix_streams : _streams)[path] = std::move(h);
    }

    // With a token set, also accept it from a cookie, so a page's own
    // subresources (scripts, a model it fetches) need not carry it. The
    // cookie is named for the port: cookies are shared across ports.
    void set_cookie_auth(bool on) { _cookie_auth = on; }
    std::string cookie_name() const { return "ss_token_" + std::to_string(_port); }
    // The Set-Cookie value a page response carries to hand the token over.
    std::string token_cookie() const {
        return cookie_name() + "=" + _token + "; Path=/; HttpOnly; SameSite=Strict";
    }

    // Non-empty: every route wants `token=<t>` in its query and answers 403
    // without it, and responses drop `Access-Control-Allow-Origin: *`, so a
    // page on another site can neither drive the server nor read it.
    void set_token(std::string t) { _token = std::move(t); }
    static std::string make_token();   // 32 hex digits from std::random_device

    // Bind + start the accept loop on a background thread. Throws on bind
    // failure (port in use etc.).
    void start(const std::string& host, int port);
    void stop();

    ~HttpServer() { stop(); }

private:
    void serve_loop();
    void start_stream(intptr_t client, const HttpRequest& req, const StreamHandler& h,
                      std::string pending);
    void reap_streams(bool all);

    struct StreamThread {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::map<std::string, Handler> _routes;
    std::map<std::string, StreamHandler> _streams;
    std::map<std::string, Handler> _prefix_routes;
    std::map<std::string, StreamHandler> _prefix_streams;
    bool _cookie_auth = false;
    int _port = 0;
    std::mutex _stream_mu;
    std::vector<StreamThread> _stream_threads;
    std::string _token;
    std::atomic<bool> _running{false};
    std::thread _thread;
    intptr_t _listen_fd = -1;
};
