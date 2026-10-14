// RemoteFiles.cpp -- see RemoteFiles.h.

#include "app/gui/RemoteFiles.h"

#include "app/gui/Storage.h"
#include "app/gui/TarWriter.h"
#include "app/webviewer/HttpServer.h"
#include "app_generated/web_viewer_bundle.h"   // kWebViewer[] (CMake-embedded)

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace gui {
namespace remote {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// RFC 6266: a plain-ASCII fallback name, and the real one percent-encoded.
std::string disposition(const std::string& kind, const std::string& name) {
    std::string ascii, encoded;
    for (unsigned char c : name) {
        ascii += (c < 0x20 || c > 0x7e || c == '"' || c == '\\') ? '_' : (char)c;
        if (std::isalnum(c) || c == '.' || c == '-' || c == '_') {
            encoded += (char)c;
        } else {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            encoded += buf;
        }
    }
    return "Content-Disposition: " + kind + "; filename=\"" + ascii +
           "\"; filename*=UTF-8''" + encoded + "\r\n";
}

std::string json_error(const std::string& what) {
    std::string out = "{\"ok\":false,\"error\":\"";
    for (char c : what) out += (c == '"' || c == '\\') ? ' ' : c;
    return out + "\"}";
}

void reply(HttpStream& stream, int status, const std::string& json) {
    if (stream.begin("application/json", status,
                     "Content-Length: " + std::to_string(json.size()) + "\r\n"))
        stream.send(json);
}

// A file from `first` to `last` inclusive.
void send_file(HttpStream& stream, const fs::path& path, uint64_t first, uint64_t last) {
    std::ifstream f(path, std::ios::binary);
    f.seekg((std::streamoff)first);
    std::vector<char> buf(1 << 20);
    uint64_t left = last + 1 - first;
    while (left > 0 && stream.alive() && f) {
        f.read(buf.data(), (std::streamsize)std::min<uint64_t>(left, buf.size()));
        const size_t got = (size_t)f.gcount();
        if (!got || !stream.send(buf.data(), got)) break;
        left -= got;
    }
}

void serve_file(const HttpRequest& req, HttpStream& stream, bool attachment) {
    const fs::path path = fs::u8path(req.get("path"));
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        reply(stream, 404, json_error("not a file: " + req.get("path")));
        return;
    }
    const uint64_t size = fs::file_size(path, ec);
    const std::string name = path.filename().u8string();
    std::string extra = "Accept-Ranges: bytes\r\n" +
                        disposition(attachment ? "attachment" : "inline", name);
    uint64_t first = 0, last = size ? size - 1 : 0;
    const std::string range = req.header("range");
    if (!range.empty()) {
        if (!parse_range(range, size, first, last)) {
            stream.begin("text/plain", 416, "Content-Range: bytes */" + std::to_string(size) +
                                                "\r\nContent-Length: 0\r\n");
            return;
        }
        extra += "Content-Range: bytes " + std::to_string(first) + "-" + std::to_string(last) +
                 "/" + std::to_string(size) + "\r\n";
    }
    const uint64_t length = size ? last + 1 - first : 0;
    extra += "Content-Length: " + std::to_string(length) + "\r\n";
    if (!stream.begin(attachment ? "application/octet-stream" : mime_type(name),
                      range.empty() ? 200 : 206, extra))
        return;
    if (length) send_file(stream, path, first, last);
}

void handle_download(const HttpRequest& req, HttpStream& stream) {
    const fs::path path = fs::u8path(req.get("path"));
    std::error_code ec;
    if (!fs::is_directory(path, ec)) {
        serve_file(req, stream, true);
        return;
    }
    const std::string name = (path.filename().empty() ? path.parent_path().filename()
                                                      : path.filename()).u8string() + ".tar";
    if (!stream.begin("application/x-tar", 200, disposition("attachment", name))) return;
    write_tar(path.u8string(),
              [&](const void* d, size_t n) { return stream.send(d, n); });
}

void handle_upload(const HttpRequest& req, HttpStream& stream,
                   const std::function<void()>& on_upload) {
    const std::string dir = req.get("dir");
    const std::string target = storage::join_inside(dir, req.get("name"));
    std::error_code ec;
    if (req.method != "POST" || dir.empty() || target.empty() || !fs::is_directory(fs::u8path(dir), ec)) {
        reply(stream, 400, json_error("expected POST with dir= (a folder) and a relative name="));
        return;
    }
    const fs::path out = fs::u8path(target);
    if (fs::exists(out, ec) && req.get("overwrite") != "1") {
        reply(stream, 409, json_error("exists"));
        return;
    }
    fs::create_directories(out.parent_path(), ec);
    // Written beside the target and renamed into place once whole, so an upload
    // cut off halfway never leaves a file that looks complete.
    std::random_device rd;
    const fs::path part = out.u8string() + ".part-" + std::to_string(rd());
    std::ofstream f(part, std::ios::binary);
    if (!f) {
        reply(stream, 500, json_error("cannot write in " + dir));
        return;
    }
    std::vector<char> buf(1 << 20);
    int64_t got_total = 0;
    for (;;) {
        const int64_t n = stream.read(buf.data(), buf.size());
        if (n <= 0) break;
        f.write(buf.data(), n);
        got_total += n;
    }
    f.close();
    if (got_total != stream.body_size() || !f) {
        fs::remove(part, ec);
        reply(stream, 400, json_error("the upload was cut off"));
        return;
    }
    fs::rename(part, out, ec);
    if (ec) {
        fs::remove(part, ec);
        reply(stream, 500, json_error("cannot move the upload into place"));
        return;
    }
    if (on_upload) on_upload();
    reply(stream, 200, "{\"ok\":true,\"bytes\":" + std::to_string(got_total) + "}");
}

HttpResponse handle_viewer(const HttpRequest& req) {
    std::string rel = req.path.substr(std::string("/viewer/").size());
    if (rel.empty()) rel = "index.html";
    for (size_t i = 0; i < kWebViewerCount; i++)
        if (rel == kWebViewer[i].path) {
            HttpResponse r;
            r.content_type = mime_type(rel);
            r.body.assign(kWebViewer[i].data, kWebViewer[i].data + kWebViewer[i].size);
            return r;
        }
    return HttpResponse::text(404, "not found");
}

}  // namespace

const char* mime_type(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    const std::string e = dot == std::string::npos ? "" : lower(name.substr(dot + 1));
    struct Row { const char* ext; const char* type; };
    static const Row kRows[] = {
        {"html", "text/html; charset=utf-8"}, {"css", "text/css"},
        {"js", "text/javascript"}, {"mjs", "text/javascript"}, {"wasm", "application/wasm"},
        {"json", "application/json"}, {"txt", "text/plain; charset=utf-8"},
        {"log", "text/plain; charset=utf-8"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"},
        {"png", "image/png"}, {"gif", "image/gif"}, {"webp", "image/webp"},
        {"svg", "image/svg+xml"}, {"mp4", "video/mp4"}, {"m4v", "video/mp4"},
        {"mov", "video/quicktime"}, {"webm", "video/webm"}, {"mkv", "video/x-matroska"},
        {"pdf", "application/pdf"},
    };
    for (const Row& r : kRows)
        if (e == r.ext) return r.type;
    return "application/octet-stream";
}

bool parse_range(const std::string& header, uint64_t size, uint64_t& first, uint64_t& last) {
    if (header.rfind("bytes=", 0) != 0 || size == 0) return false;
    const std::string spec = header.substr(6);
    if (spec.find(',') != std::string::npos) return false;   // one range only
    const size_t dash = spec.find('-');
    if (dash == std::string::npos) return false;
    const std::string a = spec.substr(0, dash), b = spec.substr(dash + 1);
    try {
        if (a.empty()) {
            const uint64_t n = std::stoull(b);
            if (n == 0) return false;
            first = n >= size ? 0 : size - n;
            last = size - 1;
        } else {
            first = std::stoull(a);
            last = b.empty() ? size - 1 : std::min<uint64_t>(std::stoull(b), size - 1);
        }
    } catch (...) {
        return false;
    }
    return first <= last && first < size;
}

void add_file_routes(HttpServer& http, std::function<void()> on_upload) {
    http.route_prefix("/viewer/", handle_viewer);
    http.route_stream("/remote/raw/",
                      [](const HttpRequest& r, HttpStream& s) { serve_file(r, s, false); },
                      true);
    http.route_stream("/remote/download/", handle_download, true);
    http.route_stream("/remote/upload",
                      [on_upload](const HttpRequest& r, HttpStream& s) {
                          handle_upload(r, s, on_upload);
                      });
}

}  // namespace remote
}  // namespace gui
