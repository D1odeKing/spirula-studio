// SfmMonitor.cpp -- see SfmMonitor.h.

#include "app/SfmMonitor.h"

#include "app/SfmProgress.h"
#include "app/webviewer/HttpServer.h"

#include "app_generated/sfm_monitor_html.h"   // kSfmMonitorHtml[] (CMake-embedded)

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace app {

namespace {

// Lines the page can still ask for; a long run prints far more, and the
// terminal it came from still has the rest.
constexpr size_t kLogRing = 5000;
constexpr size_t kLogPage = 2000;

struct LogLine {
    uint64_t n = 0;
    std::string level, text;
};

void json_str(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    out += b;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
}

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

// A file's write time as a string: the page only compares it, and the tick
// count outgrows a JavaScript number.
std::string stamp(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    return ec ? std::string("0") : std::to_string((long long)t.time_since_epoch().count());
}

bool slurp(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

template <class T>
void put(std::vector<uint8_t>& b, T v) {
    const size_t at = b.size();
    b.resize(at + sizeof v);
    std::memcpy(b.data() + at, &v, sizeof v);
}

// A `thumbs/` name from the page: an image's path under the image directory,
// so it may hold '/' but never climb out or name a drive.
bool safe_relative(const std::string& s) {
    if (s.empty() || s.size() > 1024 || s[0] == '/' || s.find('\\') != std::string::npos ||
        s.find(':') != std::string::npos || s.find('\0') != std::string::npos)
        return false;
    for (const fs::path& part : fs::path(s))
        if (part == ".." || part == ".") return false;
    return true;
}

std::string model_file(int k) {
    return k ? "model_" + std::to_string(k) + ".bin" : std::string("model.bin");
}

std::string images_file(int k) {
    return k ? "images_" + std::to_string(k) + ".bin" : std::string("images.bin");
}

}  // namespace

// ===========================================================================
// Impl
// ===========================================================================

struct SfmMonitor::Impl {
    SfmMonitorOptions opt;
    HttpServer http;
    bool started = false;

    std::mutex log_mu;
    std::deque<LogLine> ring;   // guarded by log_mu
    uint64_t next_line = 0;     // guarded by log_mu

    // The handlers run one at a time on the server thread, so these need no lock.
    int64_t status_mtime = 0;
    gui::RunStatus status;
    bool have_status = false;

    fs::path progress() const { return opt.progress_dir; }
    fs::path workspace() const { return opt.workspace; }

    // images.bin while the run writes it, then the copy the workspace keeps.
    bool image_stats(int k, std::vector<gui::ImageStat>& out) const {
        int64_t m = 0;
        if (!opt.progress_dir.empty() &&
            gui::read_image_stats((progress() / images_file(k)).string(), m, out))
            return true;
        return k == 0 && !opt.workspace.empty() &&
               gui::read_image_stats((workspace() / "image_stats.bin").string(), m, out);
    }

    gui::FitScale scale(const std::vector<gui::ImageStat>& stats) const {
        return gui::fit_scale(stats, opt.progressive, opt.error_start, opt.error_end);
    }

    HttpResponse index() {
        HttpResponse r;
        r.content_type = "text/html; charset=utf-8";
        r.body.assign(kSfmMonitorHtml, kSfmMonitorHtml + kSfmMonitorHtmlSize);
        return r;
    }

    HttpResponse handle_status() {
        if (gui::read_status(opt.progress_dir, status_mtime, status)) have_status = true;
        const gui::RunStatus& s = status;
        std::string b = "{\"have\":";
        b += have_status ? "true" : "false";
        b += ",\"stage\":" + std::to_string(s.stage);
        b += ",\"finished\":" + std::string(s.finished ? "true" : "false");
        b += ",\"partial\":" + std::string(s.partial ? "true" : "false");
        b += ",\"metric\":" + std::string(s.metric ? "true" : "false");
        b += ",\"done\":" + std::to_string(s.done);
        b += ",\"total\":" + std::to_string(s.total);
        b += ",\"registered\":" + std::to_string(s.registered);
        b += ",\"images\":" + std::to_string(s.images);
        b += ",\"points\":" + std::to_string(s.points);
        b += ",\"models\":" + std::to_string(s.models);
        b += ",\"mean_reproj\":" + num(s.mean_reproj);
        b += ",\"stamps\":{\"model\":\"" + stamp(progress() / "model.bin") + "\"";
        b += ",\"pairs\":\"" + stamp(progress() / "pairs.bin") + "\"";
        b += ",\"images\":\"" + stamp(progress() / "images.bin") + "/" +
             stamp(workspace() / "image_stats.bin") + "\"";
        b += ",\"progressive\":\"" + stamp(workspace() / "progressive.txt") + "\"}";
        {
            std::lock_guard<std::mutex> lk(log_mu);
            b += ",\"log\":" + std::to_string(next_line);
        }
        b += ",\"live\":" + std::string(opt.log ? "true" : "false") + "}";
        return HttpResponse::json(b);
    }

    HttpResponse handle_models() {
        const std::vector<uint32_t> sizes = gui::live_model_sizes(opt.progress_dir);
        std::string b = "{\"sizes\":[";
        for (size_t i = 0; i < sizes.size(); i++)
            b += (i ? "," : "") + std::to_string(sizes[i]);
        b += "]}";
        return HttpResponse::json(b);
    }

    // docs/notes/remote-monitoring.md "/model" is the layout; the page's
    // parseModel() reads it.
    HttpResponse handle_model(const HttpRequest& req) {
        const int k = std::max(0, req.get_int("k", 0));
        int64_t m = 0;
        gui::LiveModel live;
        if (!gui::read_live_model(opt.progress_dir, m, live, 0, model_file(k)))
            return HttpResponse::text(404, "no model snapshot yet");
        const ParsedDataset& ds = live.ds;
        const auto& A = ds.train_to_normalized;
        const float radius = A[0] > 0 ? A[0] : 1.0f;
        const float centre[3] = {A[3], A[7], A[11]};

        std::vector<gui::ImageStat> stats;
        std::unordered_map<uint32_t, const gui::ImageStat*> by_id;
        gui::FitScale fit;
        if (image_stats(k, stats)) {
            fit = scale(stats);
            for (const gui::ImageStat& s : stats) by_id[s.id] = &s;
        }
        float up[3] = {0, 0, 0};
        const bool has_up = !ds.gauge_oriented && gui::snapshot_up(live, up);

        const uint32_t n_cams = (uint32_t)ds.num_cameras;
        const uint32_t n_pts = (uint32_t)ds.points.num();
        HttpResponse r;
        r.content_type = "application/octet-stream";
        std::vector<uint8_t>& b = r.body;
        b.reserve(48 + (size_t)n_cams * 92 + (size_t)n_pts * 15);
        b.insert(b.end(), {'S', 'S', 'M', 'V'});
        put<uint32_t>(b, 1);
        put<uint32_t>(b, n_cams);
        put<uint32_t>(b, n_pts);
        put<uint32_t>(b, live.n_images);
        put<uint32_t>(b, (ds.gauge_oriented ? 1u : 0u) | (ds.gauge_metric ? 2u : 0u) |
                             (live.provisional ? 4u : 0u) | (has_up ? 8u : 0u));
        put<float>(b, radius);
        for (float v : up) put<float>(b, v);
        put<uint32_t>(b, 0);
        put<uint32_t>(b, 0);
        for (uint32_t i = 0; i < n_cams; i++) {
            const uint32_t id = i < live.ids.size() ? live.ids[i] : i;
            put<uint32_t>(b, id);
            for (int row = 0; row < 3; row++) {
                for (int col = 0; col < 3; col++) put<float>(b, ds.c2w[(size_t)i * 12 + row * 4 + col]);
                put<float>(b, (ds.c2w[(size_t)i * 12 + row * 4 + 3] - centre[row]) / radius);
            }
            for (int c = 0; c < 4; c++) put<float>(b, ds.intrins[(size_t)i * 4 + c]);
            put<float>(b, (float)ds.widths[i]);
            put<float>(b, (float)ds.heights[i]);
            const auto it = by_id.find(id);
            const gui::ImageStat* s = it == by_id.end() ? nullptr : it->second;
            put<float>(b, s ? gui::image_badness(*s, true, fit) : -1.0f);
            put<float>(b, s ? gui::image_badness(*s, false, fit) : -1.0f);
            put<float>(b, s ? s->mean_error : -1.0f);
            put<float>(b, s ? (float)s->points : -1.0f);
        }
        for (uint32_t i = 0; i < n_pts; i++)
            for (int c = 0; c < 3; c++)
                put<float>(b, (float)((ds.points.xyz[(size_t)i * 3 + c] - centre[c]) / radius));
        b.insert(b.end(), ds.points.rgb.begin(), ds.points.rgb.begin() + (size_t)n_pts * 3);
        return r;
    }

    // "SSPM", u32 images, bins, peak, staged; then bins*bins u32 inliers and,
    // when staged, the planned and verified planes.
    HttpResponse handle_pairs() {
        gui::PairMatrix pm;
        int64_t m = 0;
        bool ok = gui::read_pair_matrix(opt.progress_dir, m, pm);
        if (!ok && !opt.workspace.empty())
            ok = gui::read_pair_matrix_from_matches((workspace() / "matches.bin").string(), m, pm);
        if (!ok || pm.empty()) return HttpResponse::text(404, "no pair matrix yet");
        HttpResponse r;
        r.content_type = "application/octet-stream";
        std::vector<uint8_t>& b = r.body;
        b.insert(b.end(), {'S', 'S', 'P', 'M'});
        put<uint32_t>(b, pm.n_images);
        put<uint32_t>(b, pm.bins);
        put<uint32_t>(b, pm.peak);
        put<uint32_t>(b, pm.staged() ? 1u : 0u);
        auto plane = [&](const std::vector<uint32_t>& v) {
            const size_t at = b.size();
            b.resize(at + v.size() * 4);
            std::memcpy(b.data() + at, v.data(), v.size() * 4);
        };
        plane(pm.counts);
        if (pm.staged()) {
            plane(pm.planned);
            plane(pm.verified);
        }
        return r;
    }

    HttpResponse handle_images() {
        std::vector<gui::ImageStat> stats;
        if (!image_stats(0, stats)) return HttpResponse::text(404, "no image statistics yet");
        const gui::FitScale fit = scale(stats);
        std::string b = "{\"green_px\":" + num(fit.green_px) + ",\"red_px\":" + num(fit.red_px) +
                        ",\"images\":[";
        for (size_t i = 0; i < stats.size(); i++) {
            const gui::ImageStat& s = stats[i];
            if (i) b += ',';
            b += "{\"id\":" + std::to_string(s.id) + ",\"name\":";
            json_str(b, s.name);
            b += ",\"placed\":" + std::string(s.placed ? "true" : "false");
            b += ",\"keypoints\":" + std::to_string(s.keypoints);
            b += ",\"points\":" + std::to_string(s.points);
            b += ",\"mean\":" + num(s.mean_error);
            b += ",\"max\":" + num(s.max_error);
            b += ",\"bad_err\":" + num(gui::image_badness(s, true, fit));
            b += ",\"bad_pts\":" + num(gui::image_badness(s, false, fit)) + "}";
        }
        b += "]}";
        return HttpResponse::json(b);
    }

    HttpResponse handle_log(const HttpRequest& req) {
        const uint64_t since = (uint64_t)std::max(0.0, req.get_double("since", 0));
        std::lock_guard<std::mutex> lk(log_mu);
        const uint64_t first = ring.empty() ? next_line : ring.front().n;
        std::string b = "{\"first\":" + std::to_string(first);
        b += ",\"live\":" + std::string(opt.log ? "true" : "false") + ",\"lines\":[";
        size_t sent = 0;
        uint64_t next = std::max(since, first);
        for (const LogLine& l : ring) {
            if (l.n < since) continue;
            if (sent == kLogPage) break;
            if (sent++) b += ',';
            b += "{\"n\":" + std::to_string(l.n) + ",\"level\":";
            json_str(b, l.level);
            b += ",\"text\":";
            json_str(b, l.text);
            b += '}';
            next = l.n + 1;
        }
        b += "],\"next\":" + std::to_string(next) + "}";
        return HttpResponse::json(b);
    }

    HttpResponse handle_progressive() {
        HttpResponse r;
        if (opt.workspace.empty() || !slurp(workspace() / "progressive.txt", r.body))
            return HttpResponse::text(404, "no progressive.txt in this workspace");
        r.content_type = "text/plain; charset=utf-8";
        return r;
    }

    HttpResponse handle_thumb(const HttpRequest& req) {
        const std::string name = req.get("name");
        if (!safe_relative(name)) return HttpResponse::text(400, "bad name");
        const fs::path dir = progress() / "thumbs";
        fs::path p = dir / (name + ".jpg");
        if (!fs::exists(p)) p = dir / fs::path(name).replace_extension(".jpg");
        HttpResponse r;
        if (!slurp(p, r.body)) return HttpResponse::text(404, "no thumbnail");
        r.content_type = "image/jpeg";
        return r;
    }
};

// ===========================================================================
// SfmMonitor
// ===========================================================================

SfmMonitor::SfmMonitor() : _impl(new Impl) {}
SfmMonitor::~SfmMonitor() { stop(); }

std::string SfmMonitor::start(SfmMonitorOptions opt) {
    Impl& im = *_impl;
    if (opt.token.empty()) opt.token = HttpServer::make_token();
    im.opt = std::move(opt);
    im.http.set_token(im.opt.token);
    im.http.route("/", [&im](const HttpRequest&) { return im.index(); });
    im.http.route("/index.html", [&im](const HttpRequest&) { return im.index(); });
    im.http.route("/status", [&im](const HttpRequest&) { return im.handle_status(); });
    im.http.route("/models", [&im](const HttpRequest&) { return im.handle_models(); });
    im.http.route("/model", [&im](const HttpRequest& r) { return im.handle_model(r); });
    im.http.route("/pairs", [&im](const HttpRequest&) { return im.handle_pairs(); });
    im.http.route("/images", [&im](const HttpRequest&) { return im.handle_images(); });
    im.http.route("/log", [&im](const HttpRequest& r) { return im.handle_log(r); });
    im.http.route("/progressive", [&im](const HttpRequest&) { return im.handle_progressive(); });
    im.http.route("/thumb", [&im](const HttpRequest& r) { return im.handle_thumb(r); });
    im.http.start(im.opt.host, im.opt.port);
    im.started = true;
    return im.opt.token;
}

void SfmMonitor::stop() {
    Impl& im = *_impl;
    if (!im.started) return;
    im.started = false;
    im.http.stop();
}

void SfmMonitor::log(const char* level, std::string text) {
    Impl& im = *_impl;
    std::lock_guard<std::mutex> lk(im.log_mu);
    im.ring.push_back({im.next_line++, level, std::move(text)});
    if (im.ring.size() > kLogRing) im.ring.pop_front();
}

}  // namespace app
