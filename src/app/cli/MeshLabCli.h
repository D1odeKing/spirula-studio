#pragma once

// What the four mesh-lab tools (docs/meshlab.md) share on the command line:
// the help row layout, flag-value parsing that names the flag it failed on,
// and the subcommand table rows Main.cpp splices in.

#include "app/Tools.h"
#include "i18n/Message.h"
#include "i18n/catalog/MeshLab.h"
#include "mesh/MeshViews.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace app {

constexpr const char* kToolMeshEval = "mesh-eval";
constexpr const char* kToolMeshDense = "mesh-dense";
constexpr const char* kToolMeshRefine = "mesh-refine";
constexpr const char* kToolScale = "scale";

}  // namespace app

int spirula_mesh_eval_main(int argc, char** argv);
int spirula_mesh_dense_main(int argc, char** argv);
int spirula_mesh_refine_main(int argc, char** argv);
int spirula_scale_main(int argc, char** argv);

// Rows of Main.cpp's tools() table.
#define SS_MESHLAB_TOOL_ROWS                                                                      \
    {app::kToolMeshEval, &spirula::i18n::msg::meshlab::eval_tagline, spirula_mesh_eval_main},     \
    {app::kToolMeshDense, &spirula::i18n::msg::meshlab::dense_tagline, spirula_mesh_dense_main},  \
    {app::kToolMeshRefine, &spirula::i18n::msg::meshlab::refine_tagline, spirula_mesh_refine_main}, \
    {app::kToolScale, &spirula::i18n::msg::meshlab::scale_tagline, spirula_scale_main},

namespace meshlab_cli {

namespace M = spirula::i18n::msg::meshlab;

inline void help_row(const char* flags, const spirula::i18n::Msg& m, int col = 30) {
    std::string left = std::string("    ") + flags;
    if (spirula::i18n::display_width(left) >= col + 4) {
        std::fprintf(stderr, "%s\n", left.c_str());
        left.clear();
    }
    left = spirula::i18n::pad_to(left, col + 4);
    for (const std::string& line : spirula::i18n::wrap(m.get(), 86 - col - 4)) {
        std::fprintf(stderr, "%s%s\n", left.c_str(), line.c_str());
        left.assign((size_t)col + 4, ' ');
    }
}

// `args` is the usage after the program name.
inline void intro(const std::string& args, const spirula::i18n::Msg& tagline, const spirula::i18n::Msg& text) {
    const std::string prog = app::program_name();
    std::fprintf(stderr, "%s -- %s\n\n    %s %s\n\n", prog.c_str(), tagline.get(), prog.c_str(), args.c_str());
    for (const std::string& l : spirula::i18n::wrap(text.get(), 80)) std::fprintf(stderr, "    %s\n", l.c_str());
    std::fprintf(stderr, "\n%s\n", M::label_options.get());
}

inline void outro() { std::fprintf(stderr, "\n%s --lang <code>\n", M::label_common.get()); }

// Thrown by the parsers below; main prints it and returns 2.
struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Args {
public:
    Args(int argc, char** argv) : argc_(argc), argv_(argv) {}
    bool more() const { return i_ + 1 < argc_; }
    std::string next() { return argv_[++i_]; }
    const std::string& flag() const { return flag_; }
    void set_flag(const std::string& f) { flag_ = f; }
    std::string value() {
        if (!more()) throw UsageError(spirula::i18n::format(M::err_needs_value, {flag_}));
        return next();
    }
    double number() {
        const std::string v = value();
        char* end = nullptr;
        const double x = std::strtod(v.c_str(), &end);
        if (end == v.c_str() || *end) throw UsageError(spirula::i18n::format(M::err_bad_value, {v, flag_}));
        return x;
    }
    long long integer() {
        const std::string v = value();
        char* end = nullptr;
        const long long x = std::strtoll(v.c_str(), &end, 10);
        if (end == v.c_str() || *end) throw UsageError(spirula::i18n::format(M::err_bad_value, {v, flag_}));
        return x;
    }
    // "x y z" in one argument, or three.
    void point(double out[3]) {
        std::string v = value();
        std::vector<double> xs;
        for (;;) {
            const char* s = v.c_str();
            char* end = nullptr;
            while (*s) {
                const double x = std::strtod(s, &end);
                if (end == s) throw UsageError(spirula::i18n::format(M::err_bad_value, {v, flag_}));
                xs.push_back(x);
                s = end;
                while (*s == ' ' || *s == ',') ++s;
            }
            if (xs.size() >= 3 || !more()) break;
            v = next();
        }
        if (xs.size() != 3) throw UsageError(spirula::i18n::format(M::err_bad_value, {v, flag_}));
        for (int k = 0; k < 3; ++k) out[k] = xs[(size_t)k];
    }
private:
    int argc_;
    char** argv_;
    int i_ = 0;
    std::string flag_;
};

inline std::vector<double> parse_list(const std::string& csv, const std::string& flag) {
    std::vector<double> out;
    const char* s = csv.c_str();
    while (*s) {
        char* end = nullptr;
        const double x = std::strtod(s, &end);
        if (end == s) throw UsageError(spirula::i18n::format(M::err_bad_value, {csv, flag}));
        out.push_back(x);
        s = end;
        while (*s == ',' || *s == ' ') ++s;
    }
    return out;
}

inline std::string num(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

// Six significant digits whatever the scale: a dataset unit may be a metre or a millimetre.
inline std::string sig(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

// Selects the normals and depths folders of `opt` when given; true when `f` was one.
inline bool view_dir_flag(const std::string& f, Args& a, meshing::ViewOptions& opt) {
    if (f == "--normals") opt.normal_dir = a.value();
    else if (f == "--depths") opt.depth_dir = a.value();
    else return false;
    return true;
}

inline void print_maps(const meshing::ViewSet& vs) {
    long long normals = 0, depths = 0;
    for (const auto& v : vs.views) {
        normals += !v.normal.empty();
        depths += !v.depth.empty();
    }
    std::printf("%s\n", spirula::i18n::format(M::log_maps, {normals, depths, (long long)vs.views.size()}).c_str());
}

}  // namespace meshlab_cli
