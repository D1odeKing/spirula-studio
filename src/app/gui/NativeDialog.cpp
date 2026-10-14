// NativeDialog.cpp -- see NativeDialog.h.

#include "app/gui/NativeDialog.h"
#include "app/gui/RemoteDisplay.h"

#include "app/gui/Subprocess.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#endif

namespace fs = std::filesystem;

namespace gui {

#ifdef __APPLE__
// NativeDialogMac.mm -- the one piece that has to be Objective-C.
std::vector<std::string> mac_pick(const std::string& title, bool folder,
                                  bool also_folders,
                                  const std::vector<std::string>& extensions,
                                  const std::string& start_dir, bool multi);
std::vector<std::string> mac_save(const std::string& title,
                                  const std::vector<std::string>& extensions,
                                  const std::string& start_dir,
                                  const std::string& suggested);
#endif

namespace {

struct Request {
    std::string title;
    NativeDialog::Mode mode = NativeDialog::Mode::Folder;
    std::vector<std::string> extensions;
    std::string start_dir;
    bool multi = false;
    std::string suggested;
};

#ifdef _WIN32

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                      nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(const wchar_t* s) {
    if (!s || !*s) return {};
    // The -1 form counts and writes the terminator, so the buffer is sized for
    // it and the terminator is dropped afterwards rather than written past the
    // end.
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr,
                                      nullptr);
    if (n <= 1) return {};
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    out.pop_back();
    return out;
}

std::vector<std::string> platform_pick(const Request& r, NativeDialog::Job&) {
    std::vector<std::string> out;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                                   COINIT_DISABLE_OLE1DDE);
    const bool saving = r.mode == NativeDialog::Mode::Save;
    IFileDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(saving ? CLSID_FileSaveDialog
                                       : CLSID_FileOpenDialog,
                                nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg)))) {
        if (SUCCEEDED(co)) CoUninitialize();
        return out;
    }

    DWORD opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
    if (r.mode == NativeDialog::Mode::Folder) opts |= FOS_PICKFOLDERS;
    else if (saving)                          opts |= FOS_OVERWRITEPROMPT;
    else if (r.multi)                         opts |= FOS_ALLOWMULTISELECT;
    dlg->SetOptions(opts);
    if (saving && !r.suggested.empty())
        dlg->SetFileName(widen(r.suggested).c_str());
    // A name typed without its extension gets the first one offered.
    if (saving && !r.extensions.empty() && r.extensions[0].size() > 1)
        dlg->SetDefaultExtension(widen(r.extensions[0].substr(1)).c_str());

    const std::wstring title = widen(r.title);
    if (!title.empty()) dlg->SetTitle(title.c_str());

    // Storage for the filter strings, which COMDLG_FILTERSPEC only points at.
    const std::wstring supported = L"Supported files";
    const std::wstring all_label = L"All files";
    const std::wstring any = L"*.*";
    std::wstring pattern;
    std::vector<COMDLG_FILTERSPEC> specs;
    if (r.mode != NativeDialog::Mode::Folder && !r.extensions.empty()) {
        std::string p;
        for (const std::string& e : r.extensions) {
            if (!p.empty()) p += ";";
            p += "*" + e;
        }
        pattern = widen(p);
        specs.push_back({supported.c_str(), pattern.c_str()});
        specs.push_back({all_label.c_str(), any.c_str()});
        dlg->SetFileTypes((UINT)specs.size(), specs.data());
    }

    if (!r.start_dir.empty()) {
        // The shell parses backslashes only; a path joined here may mix both.
        std::string dir = r.start_dir;
        std::replace(dir.begin(), dir.end(), '/', '\\');
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(widen(dir).c_str(),
                                                  nullptr,
                                                  IID_PPV_ARGS(&item)))) {
            dlg->SetFolder(item);
            item->Release();
        }
    }

    if (SUCCEEDED(dlg->Show(nullptr))) {
        if (saving) {
            IShellItem* it = nullptr;
            if (SUCCEEDED(dlg->GetResult(&it))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    out.push_back(narrow(path));
                    CoTaskMemFree(path);
                }
                it->Release();
            }
            dlg->Release();
            if (SUCCEEDED(co)) CoUninitialize();
            return out;
        }
        IShellItemArray* items = nullptr;
        IFileOpenDialog* open_dlg = nullptr;
        if (SUCCEEDED(dlg->QueryInterface(IID_PPV_ARGS(&open_dlg))) &&
            SUCCEEDED(open_dlg->GetResults(&items))) {
            DWORD n = 0;
            items->GetCount(&n);
            for (DWORD i = 0; i < n; i++) {
                IShellItem* it = nullptr;
                if (FAILED(items->GetItemAt(i, &it))) continue;
                PWSTR path = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    out.push_back(narrow(path));
                    CoTaskMemFree(path);
                }
                it->Release();
            }
            items->Release();
        }
        if (open_dlg) open_dlg->Release();
    }
    dlg->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    return out;
}

bool platform_available() { return true; }

#elif defined(__APPLE__)

std::vector<std::string> platform_pick(const Request& r, NativeDialog::Job&) {
    if (r.mode == NativeDialog::Mode::Save)
        return mac_save(r.title, r.extensions, r.start_dir, r.suggested);
    return mac_pick(r.title, r.mode == NativeDialog::Mode::Folder,
                    r.mode == NativeDialog::Mode::FileOrFolder, r.extensions,
                    r.start_dir, r.multi);
}

bool platform_available() { return true; }

#else

// The helper this desktop has, cached: the answer needs a PATH search per
// candidate and cannot change while the program runs.
const std::string& linux_helper() {
    static const std::string cached = [] {
        const char* desk = std::getenv("XDG_CURRENT_DESKTOP");
        const std::string d = desk ? desk : "";
        const bool kde = d.find("KDE") != std::string::npos ||
                         d.find("LXQt") != std::string::npos;
        // Both route through xdg-desktop-portal on a current desktop, so this
        // picks which toolkit draws it rather than which backend answers.
        const char* order_kde[] = {"kdialog", "zenity", "qarma", "matedialog"};
        const char* order_gtk[] = {"zenity", "kdialog", "qarma", "matedialog"};
        for (const char* c : kde ? order_kde : order_gtk)
            if (command_exists(c)) return std::string(c);
        return std::string();
    }();
    return cached;
}

std::vector<std::string> platform_pick(const Request& r,
                                       NativeDialog::Job& job) {
    const std::string& helper = linux_helper();
    std::vector<std::string> argv{helper};
    const bool folder = r.mode == NativeDialog::Mode::Folder;
    const bool saving = r.mode == NativeDialog::Mode::Save;
    // Where a save opens: the folder plus the name it suggests.
    std::string save_at = r.start_dir;
    if (saving) {
        if (save_at.empty()) save_at = ".";
        save_at += "/" + (r.suggested.empty() ? std::string("untitled")
                                              : r.suggested);
    }

    if (helper == "kdialog") {
        argv.push_back("--title");
        argv.push_back(r.title);
        if (saving) {
            argv.push_back("--getsavefilename");
            argv.push_back(save_at);
            if (!r.extensions.empty()) {
                std::string pat;
                for (const std::string& e : r.extensions) {
                    if (!pat.empty()) pat += " ";
                    pat += "*" + e;
                }
                argv.push_back(pat + "|" + r.title);
            }
        } else if (folder) {
            argv.push_back("--getexistingdirectory");
            argv.push_back(r.start_dir.empty() ? "." : r.start_dir);
        } else {
            if (r.multi) {
                argv.push_back("--multiple");
                argv.push_back("--separate-output");
            }
            argv.push_back("--getopenfilename");
            argv.push_back(r.start_dir.empty() ? "." : r.start_dir);
            if (!r.extensions.empty()) {
                std::string pat;
                for (const std::string& e : r.extensions) {
                    if (!pat.empty()) pat += " ";
                    pat += "*" + e;
                }
                argv.push_back(pat + "|" + r.title);
            }
        }
    } else {
        argv.push_back("--file-selection");
        argv.push_back("--title=" + r.title);
        if (folder) argv.push_back("--directory");
        if (saving) {
            argv.push_back("--save");
            argv.push_back("--confirm-overwrite");
            argv.push_back("--filename=" + save_at);
        } else if (!r.start_dir.empty()) {
            // The trailing separator is what makes zenity read it as the
            // folder to open in rather than a file name to preselect.
            argv.push_back("--filename=" + r.start_dir + "/");
        }
        if (!folder && r.multi) {
            argv.push_back("--multiple");
            argv.push_back("--separator=\n");
        }
        if (!folder && !r.extensions.empty()) {
            std::string pat;
            for (const std::string& e : r.extensions) {
                if (!pat.empty()) pat += " ";
                pat += "*" + e;
            }
            argv.push_back("--file-filter=" + r.title + " | " + pat);
            argv.push_back("--file-filter=* | *");
        }
    }

    // stderr is merged into stdout by run_process, and both helpers are prone
    // to toolkit warnings, so a line only counts if it names something that
    // exists.
    std::vector<std::string> out;
    run_process(argv, "", [&out, saving](const std::string& line) {
        std::error_code ec;
        if (line.empty() || line[0] != '/') return;
        // A save names a file that does not exist yet, so what has to be
        // there is its folder.
        if (saving ? fs::is_directory(fs::path(line).parent_path(), ec)
                   : fs::exists(line, ec))
            out.push_back(line);
    }, job.cancel);
    return out;
}

bool platform_available() { return !linux_helper().empty(); }

#endif

}  // namespace

// Quitting must not wait on a picker the user has walked away from, so the
// worker is abandoned rather than joined: it holds its own reference to the
// Job it writes into, and nothing here outlives this object.
NativeDialog::~NativeDialog() {
    if (_job) _job->cancel = true;
    if (_worker.joinable()) _worker.detach();
}

// A remote session's picker would open on the pod's invisible screen.
bool NativeDialog::available() { return !remote::active() && platform_available(); }

bool NativeDialog::open(const std::string& title, Mode mode,
                        const std::vector<std::string>& extensions,
                        const std::string& start_dir, bool multi,
                        const std::string& suggested) {
    if (_job || !available()) return false;
    if (_worker.joinable()) _worker.detach();   // a previous, abandoned picker

    Request r;
    r.title = title;
    r.mode = mode;
    r.extensions = extensions;
    r.start_dir = start_dir;
    r.multi = multi && mode == Mode::File;
    r.suggested = suggested;

    _results.clear();
    auto job = std::make_shared<Job>();
    _job = job;

#ifdef __APPLE__
    // NSOpenPanel is main-thread only; this call blocks until it closes.
    job->picked = platform_pick(r, *job);
    job->done = true;
#else
    _worker = std::thread([job, r] {
        job->picked = platform_pick(r, *job);
        job->done = true;
    });
#endif
    return true;
}

bool NativeDialog::poll() {
    if (!_job || !_job->done) return false;
    if (_worker.joinable()) _worker.join();
    _results.swap(_job->picked);
    _job.reset();
    return true;
}

}  // namespace gui
