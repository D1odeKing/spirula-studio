#include "core/LicenseConsent.h"

#include "core/LicenseFamilies.h"
#include "core/LicenseTexts.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>

namespace fs = std::filesystem;

namespace spirula::license {
namespace {

const char* kKey = "accepted_license=";

// What a gui.conf line is, minus the line ending.
std::vector<std::string> read_lines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream in(path, std::ios::binary);
    for (std::string s; std::getline(in, s);) {
        while (!s.empty() && s.back() == '\r') s.pop_back();
        lines.push_back(s);
    }
    return lines;
}

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v && *v ? v : "";
}

const Terms* const* builtin_terms(size_t& n) {
    static const Terms kSam3{
        family::kSam3, "SAM License (Meta)",
        "https://github.com/facebookresearch/sam3/blob/main/LICENSE", kSam3License};
    static const Terms kSam2{
        family::kSam2, "Apache License 2.0 (SAM 2.1, Meta)",
        "https://github.com/facebookresearch/sam2/blob/main/LICENSE", kSam2Apache};
    static const Terms kGdino{
        family::kGdino, "Apache License 2.0 (Grounding DINO, IDEA Research)",
        "https://github.com/IDEA-Research/GroundingDINO/blob/main/LICENSE", kGdinoApache};
    static const Terms kBirefnet{
        family::kBirefnet, "MIT License (BiRefNet)",
        "https://github.com/ZhengPeng7/BiRefNet/blob/main/LICENSE", kBirefnetMit};
    static const Terms* const kAll[] = {&kSam3, &kSam2, &kGdino, &kBirefnet};
    n = sizeof kAll / sizeof kAll[0];
    return kAll;
}

std::mutex g_registry_mu;
std::vector<const Terms*>& registered() {
    static std::vector<const Terms*> v;
    return v;
}

}  // namespace

const Terms* terms_for(const std::string& family) {
    size_t n = 0;
    const Terms* const* all = builtin_terms(n);
    for (size_t i = 0; i < n; ++i)
        if (family == all[i]->family) return all[i];
    std::lock_guard<std::mutex> lock(g_registry_mu);
    for (const Terms* t : registered())
        if (family == t->family) return t;
    return nullptr;
}

void register_terms(const Terms* t) {
    if (!t || !t->family || terms_for(t->family)) return;
    std::lock_guard<std::mutex> lock(g_registry_mu);
    registered().push_back(t);
}

std::string known_families() {
    size_t n = 0;
    const Terms* const* all = builtin_terms(n);
    std::string out;
    for (size_t i = 0; i < n; ++i) out += (i ? ", " : "") + std::string(all[i]->family);
    std::lock_guard<std::mutex> lock(g_registry_mu);
    for (const Terms* t : registered()) out += ", " + std::string(t->family);
    return out;
}

std::vector<std::string> split_families(const std::string& list) {
    std::vector<std::string> out;
    std::stringstream ss(list);
    for (std::string t; std::getline(ss, t, ',');) {
        const size_t a = t.find_first_not_of(" \t"), b = t.find_last_not_of(" \t");
        if (a != std::string::npos) out.push_back(t.substr(a, b - a + 1));
    }
    return out;
}

std::string scratch_path_for(const std::string& path) {
    static std::atomic<unsigned> counter{0};
    std::random_device rd;
    char tag[40];
    std::snprintf(tag, sizeof tag, ".%08x%08x.%u.tmp", rd(), rd(), counter.fetch_add(1));
    return path + tag;
}

std::string settings_path() {
#ifdef _WIN32
    fs::path dir = env_or_empty("APPDATA");
#else
    fs::path dir = env_or_empty("XDG_CONFIG_HOME");
    if (dir.empty()) {
        const std::string home = env_or_empty("HOME");
        if (!home.empty()) dir = fs::path(home) / ".config";
    }
#endif
    if (dir.empty()) return std::string();
    // An existing spirulae-splat/ from before the rename is adopted where it is.
    std::error_code ec;
    fs::path app = dir / "spirula-studio";
    if (!fs::exists(app, ec) && fs::is_directory(dir / "spirulae-splat", ec))
        app = dir / "spirulae-splat";
    return (app / "gui.conf").string();
}

std::vector<std::string> accepted_all() {
    std::vector<std::string> out;
    const std::string path = settings_path();
    if (path.empty()) return out;
    for (const std::string& l : read_lines(path))
        if (l.rfind(kKey, 0) == 0 && l.size() > std::char_traits<char>::length(kKey)) {
            const std::string f = l.substr(std::char_traits<char>::length(kKey));
            if (std::find(out.begin(), out.end(), f) == out.end()) out.push_back(f);
        }
    return out;
}

bool accepted(const std::string& family) {
    const auto all = accepted_all();
    return std::find(all.begin(), all.end(), family) != all.end();
}

std::vector<std::string> missing(const std::string& families) {
    std::vector<std::string> out;
    for (const std::string& t : split_families(families))
        if (!accepted(t)) out.push_back(t);
    return out;
}

std::vector<std::string> unique_families(const std::vector<std::string>& lists) {
    std::vector<std::string> out;
    for (const std::string& list : lists)
        for (const std::string& t : split_families(list))
            if (std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
    return out;
}

bool accept_enabled(const std::string& family, bool ticked) {
    return ticked && terms_for(family) != nullptr;
}

bool record(const std::string& family) {
    if (!terms_for(family) || settings_path().empty()) return false;
    if (accepted(family)) return true;
    const fs::path path = settings_path();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::vector<std::string> lines = read_lines(path.string());
    lines.push_back(kKey + family);
    // Written beside and renamed over, so a crash cannot leave half a settings file.
    const fs::path tmp = scratch_path_for(path.string());
    bool written = false;
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        for (const std::string& l : lines) o << l << '\n';
        o.flush();
        written = o.good();
    }
    if (written) fs::rename(tmp, path, ec);
    if (!written || ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return accepted(family);
}

}  // namespace spirula::license
