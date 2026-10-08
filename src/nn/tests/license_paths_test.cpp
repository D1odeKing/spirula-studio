// The consent store (core/LicenseConsent) must read the gui.conf the GUI writes,
// i.e. the path app::config_dir() gives it -- two copies of that logic, pinned
// equal here -- and must fail closed where app::config_dir() falls back to ".".

#include "app/AppPaths.h"
#include "core/LicenseConsent.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef _WIN32
#define SS_SETENV(k, v) _putenv_s(k, v)
#define SS_UNSETENV(k) _putenv_s(k, "")
#else
#include <unistd.h>
#define SS_SETENV(k, v) setenv(k, v, 1)
#define SS_UNSETENV(k) unsetenv(k)
#endif

namespace fs = std::filesystem;
namespace lic = spirula::license;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_failures++;
}

void same(const char* what) {
    const std::string a = (fs::path(app::config_dir()) / "gui.conf").string();
    const std::string b = lic::settings_path();
    check(a == b, std::string("same path: ") + what + (a == b ? "" : "  app=" + a + "  lic=" + b));
}

#ifdef _WIN32
const char* kConfigVar = "APPDATA";
#else
const char* kConfigVar = "XDG_CONFIG_HOME";
#endif

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "spirula_license_paths_test";
    std::error_code ec;
    fs::remove_all(root, ec);

    fs::create_directories(root / "x");
    SS_SETENV(kConfigVar, (root / "x").string().c_str());
    same("config home set, nothing under it");

    fs::remove_all(root, ec);
    fs::create_directories(root / "x" / "spirulae-splat");
    same("only the pre-rename spirulae-splat/ exists (adopted)");

    fs::create_directories(root / "x" / "spirula-studio");
    same("both exist (the new name wins)");

#ifndef _WIN32
    SS_UNSETENV(kConfigVar);
    fs::create_directories(root / "h");
    SS_SETENV("HOME", (root / "h").string().c_str());
    same("only HOME set (~/.config)");

    SS_SETENV(kConfigVar, "");
    same("config home set but empty");

    // Fail closed: nothing to anchor the file to.
    SS_UNSETENV(kConfigVar);
    SS_UNSETENV("HOME");
    fs::create_directories(root / "cwd");
    fs::current_path(root / "cwd");
    check(lic::settings_path().empty(), "no config home and no HOME: no settings path");
    check(!lic::accepted("sam3") && lic::accepted_all().empty(), "...so nothing is accepted");
    check(!lic::record("sam3"), "...and record() refuses");
    check(fs::is_empty(root / "cwd"), "...and nothing was written to the working directory");
    // A gui.conf lying in the working directory is not consent either.
    fs::create_directories(root / "cwd" / "spirula-studio");
    { FILE* f = std::fopen("spirula-studio/gui.conf", "w"); std::fputs("accepted_license=sam3\n", f); std::fclose(f); }
    check(!lic::accepted("sam3"), "a gui.conf in the working directory is not read");
    fs::current_path("/");
#endif
    fs::remove_all(root, ec);
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
