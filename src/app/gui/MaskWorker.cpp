// Masking several camera folders at once (MaskWorker.h).

#include "app/gui/MaskWorker.h"

#include "app/FrameMask.h"
#include "app/gui/Subprocess.h"
#include "data/Json.h"
#include "data/JsonWrite.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>

namespace gui {

namespace {

void write_stencil(JsonWriter& w, const app::CameraStencil& c) {
    w.field("shapes", app::format_mask_shapes(c.mask.shapes));
    w.field("image", c.mask.image);
    w.field("detect_border", c.detect_border);
    w.field("shrink", c.shrink);
}

bool read_stencil(const JsonValue& v, app::CameraStencil& c, std::string& error) {
    const JsonValue* s = v.find("shapes");
    if (s && !s->as_string().empty() &&
        !app::parse_mask_shapes(s->as_string(), c.mask.shapes, error))
        return false;
    if (const JsonValue* i = v.find("image")) c.mask.image = i->as_string();
    if (const JsonValue* b = v.find("detect_border")) c.detect_border = b->as_bool();
    if (const JsonValue* k = v.find("shrink")) c.shrink = (float)k->as_double(c.shrink);
    return true;
}

std::string str(const JsonValue& v, const char* k) {
    const JsonValue* x = v.find(k);
    return x ? x->as_string() : std::string();
}
double num(const JsonValue& v, const char* k, double def) {
    const JsonValue* x = v.find(k);
    return x ? x->as_double(def) : def;
}
bool flag(const JsonValue& v, const char* k, bool def) {
    const JsonValue* x = v.find(k);
    return x ? x->as_bool(def) : def;
}

// Free memory of the GPU `device` names (an ordinal, or the first), in MiB;
// -1 when nvidia-smi is not there to ask.
long long free_vram_mib(const std::string& device) {
    std::vector<long long> free;
    std::atomic<bool> never{false};
    const int rc = run_process(
        {"nvidia-smi", "--query-gpu=memory.free", "--format=csv,noheader,nounits"}, "",
        [&](const std::string& line) {
            try { free.push_back(std::stoll(line)); } catch (...) {}
        },
        never);
    if (rc != 0 || free.empty()) return -1;
    size_t i = 0;
    if (!device.empty() && std::all_of(device.begin(), device.end(), ::isdigit))
        i = std::min((size_t)std::stoul(device), free.size() - 1);
    return free[i];
}

}  // namespace

std::string encode_mask_task(const MaskTask& t) {
    const PrepJob& j = t.job;
    const PrepInput& in = t.input;
    JsonWriter w;
    w.object();
    w.field("images", t.images);
    w.field("masks", t.masks);
    w.field("feature_masks", t.feature_masks);
    w.field("train", t.train);
    w.key("job").object();
    w.field("workspace", j.workspace);
    w.field("resume", j.resume);
    w.field("redo_masks", j.redo_masks);
    w.field("device", j.device);
    w.field("force_external_decode", j.force_external_decode);
    w.field("image_gamut", j.image_gamut);
    w.field("image_is_linear", j.image_is_linear ? (*j.image_is_linear ? 1 : 0) : -1);
    w.field("image_exposure", j.image_exposure);
    w.field("mask_prompt", j.mask_prompt);
    w.field("mask_negative_prompt", j.mask_negative_prompt);
    w.field("mask_feature_prompt", j.mask_feature_prompt);
    w.field("mask_keep_subject", j.mask_keep_subject);
    w.field("mask_dilate_ratio", j.mask_dilate_ratio);
    w.field("mask_max_image_size", j.mask_max_image_size);
    w.field("mask_threshold", j.mask_threshold);
    w.field("mask_nms", j.mask_nms);
    w.field("mask_memory", j.mask_memory);
    w.field("mask_detect_every", j.mask_detect_every);
    w.field("mask_memory_frames", j.mask_memory_frames);
    w.field("mask_model_path", j.mask_model_path);
    w.field("mask_detector_path", j.mask_detector_path);
    w.field("mask_detector_threshold", j.mask_detector_threshold);
    w.end();
    w.key("input").object();
    w.field("path", in.path);
    w.field("is_video", in.is_video);
    w.field("subdir", in.subdir);
    w.field("packed_lenses", in.packed_lenses);
    w.key("stencil").object();
    write_stencil(w, in.stencil);
    w.key("cameras").object();
    for (const auto& [name, c] : in.stencil.cameras) {
        w.key(name.c_str()).object();
        write_stencil(w, c);
        w.end();
    }
    w.end();
    w.end();
    w.end();
    w.end();
    return w.str();
}

bool decode_mask_task(const std::string& json, MaskTask& t, std::string& error) {
    JsonValue v;
    try {
        v = json_parse(json);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    const JsonValue* j = v.find("job");
    const JsonValue* in = v.find("input");
    if (!v.is_object() || !j || !in) {
        error = "not a mask task";
        return false;
    }
    t.images = str(v, "images");
    t.masks = str(v, "masks");
    t.feature_masks = str(v, "feature_masks");
    t.train = flag(v, "train", true);
    PrepJob& p = t.job;
    p.workspace = str(*j, "workspace");
    p.resume = flag(*j, "resume", p.resume);
    p.redo_masks = flag(*j, "redo_masks", p.redo_masks);
    p.device = str(*j, "device");
    p.force_external_decode = flag(*j, "force_external_decode", false);
    p.image_gamut = str(*j, "image_gamut");
    const int lin = (int)num(*j, "image_is_linear", -1);
    if (lin >= 0) p.image_is_linear = lin == 1;
    p.image_exposure = str(*j, "image_exposure");
    p.mask_prompt = str(*j, "mask_prompt");
    p.mask_negative_prompt = str(*j, "mask_negative_prompt");
    p.mask_feature_prompt = str(*j, "mask_feature_prompt");
    p.mask_keep_subject = flag(*j, "mask_keep_subject", false);
    p.mask_dilate_ratio = (float)num(*j, "mask_dilate_ratio", p.mask_dilate_ratio);
    p.mask_max_image_size = (int)num(*j, "mask_max_image_size", p.mask_max_image_size);
    p.mask_threshold = (float)num(*j, "mask_threshold", p.mask_threshold);
    p.mask_nms = (float)num(*j, "mask_nms", p.mask_nms);
    p.mask_memory = flag(*j, "mask_memory", p.mask_memory);
    p.mask_detect_every = (int)num(*j, "mask_detect_every", p.mask_detect_every);
    p.mask_memory_frames = (int)num(*j, "mask_memory_frames", p.mask_memory_frames);
    p.mask_model_path = str(*j, "mask_model_path");
    p.mask_detector_path = str(*j, "mask_detector_path");
    p.mask_detector_threshold =
        (float)num(*j, "mask_detector_threshold", p.mask_detector_threshold);
    PrepInput& i = t.input;
    i.path = str(*in, "path");
    i.is_video = flag(*in, "is_video", false);
    i.subdir = str(*in, "subdir");
    i.packed_lenses = (int)num(*in, "packed_lenses", 0);
    if (const JsonValue* s = in->find("stencil")) {
        if (!read_stencil(*s, i.stencil, error)) return false;
        if (const JsonValue* cams = s->find("cameras"); cams && cams->is_object())
            for (const auto& [name, c] : cams->obj)
                if (!read_stencil(c, i.stencil.cameras[name], error)) return false;
    }
    return true;
}

bool parse_worker_progress(const std::string& line, long long& done, long long& total) {
    return std::sscanf(line.c_str(), "@@progress %lld %lld", &done, &total) == 2;
}

int mask_stream_count(int requested, size_t folders, const std::string& device) {
    if (folders <= 1) return 1;
    long long n = requested;
    if (n <= 0) {
        const long long free = free_vram_mib(device);
        n = free < 0 ? 1 : std::min(8LL, free / 3072);
    }
    return (int)std::max(1LL, std::min(n, (long long)folders));
}

int mask_worker_main(const std::string& job_file) {
    std::ifstream f(job_file, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    MaskTask t;
    std::string error;
    if (!f || !decode_mask_task(ss.str(), t, error)) {
        std::printf("@@error %s\n", error.empty() ? "cannot read the task" : error.c_str());
        return 1;
    }
    RunProgress prog;
    std::atomic<bool> cancel{false};
    DatasetPrep dp(&prog, RunFilms{}, cancel);
    bool ok = false, folded = false;
    std::atomic<bool> finished{false};
    std::thread work([&] {
        ok = dp.mask_one(t.job, t.input, t.images, t.masks, t.feature_masks, t.train, folded,
                         error);
        finished = true;
    });
    // The parent reads lines: progress twice a second, and the log as it comes.
    auto report = [&] {
        for (const RunLine& l : prog.drain())
            std::printf("@@log %d %s\n", l.detail ? 1 : 0, l.text.c_str());
        const StageProgress s = prog.stage(Stage::Masks);
        std::printf("@@progress %lld %lld\n", (long long)s.done, (long long)s.total);
        std::fflush(stdout);
    };
    while (!finished) {
        report();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    work.join();
    report();
    if (!ok) {
        std::printf("@@error %s\n", error.c_str());
        return 1;
    }
    std::printf("@@folded %d\n", folded ? 1 : 0);
    return 0;
}

}  // namespace gui
