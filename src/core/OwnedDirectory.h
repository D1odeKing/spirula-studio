#pragma once

#include <filesystem>
#include <stdexcept>

namespace spirula {

inline void remove_owned_directory(const std::filesystem::path& directory, const std::filesystem::path& parent,
                                    const std::string& name) {
    namespace fs = std::filesystem;
    if (!fs::exists(directory)) return;
    if (name.empty() || name == "." || name == ".." || fs::path(name).filename() != name ||
        fs::absolute(directory).lexically_normal() != fs::absolute(parent / name).lexically_normal())
        throw std::runtime_error("invalid owned cleanup directory");
    const auto resolved_parent = fs::canonical(parent), resolved = fs::canonical(directory);
    if (resolved_parent != fs::absolute(parent).lexically_normal() || resolved != resolved_parent / name)
        throw std::runtime_error("owned cleanup directory or parent is redirected");
    for (const auto& entry : fs::recursive_directory_iterator(directory)) {
        const auto relative = entry.path().lexically_relative(directory);
        if (entry.is_symlink() || fs::canonical(entry.path()) != resolved / relative)
            throw std::runtime_error("owned cleanup directory contains a redirected path");
    }
    fs::remove_all(resolved);
}

}  // namespace spirula
