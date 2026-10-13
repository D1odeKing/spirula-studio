#pragma once
// Paths past Windows' 260-character MAX_PATH. The executable's manifest opts into long
// paths, but Windows honours that only where the LongPathsEnabled policy is set, which
// it is not by default; the extended-length form works either way.

#include <filesystem>
#include <string>

namespace spirula {

// Absolute and normal; on Windows also extended-length (\\?\C:\..., \\?\UNC\host\...).
// Windows takes such a path literally, so it never holds '/', '.' or '..'.
inline std::filesystem::path long_path(const std::filesystem::path& path) {
    namespace fs = std::filesystem;
#ifdef _WIN32
    const std::wstring& native = path.native();
    if (native.rfind(L"\\\\?\\", 0) == 0) return fs::path(native).lexically_normal();
    const std::wstring full = fs::absolute(path).lexically_normal().native();
    if (full.rfind(L"\\\\", 0) == 0) return fs::path(L"\\\\?\\UNC\\" + full.substr(2));
    return fs::path(L"\\\\?\\" + full);
#else
    return fs::absolute(path).lexically_normal();
#endif
}

}  // namespace spirula
