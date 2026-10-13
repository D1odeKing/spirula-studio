#pragma once

#include "core/LongPath.h"

#include <filesystem>
#include <stdexcept>

namespace spirula {

inline void remove_owned_directory(const std::filesystem::path& directory, const std::filesystem::path& parent,
                                    const std::string& name) {
    namespace fs = std::filesystem;
    const auto owned = long_path(directory), base = long_path(parent);
    if (!fs::exists(owned)) return;
    if (name.empty() || name == "." || name == ".." || fs::path(name).filename() != name || owned != long_path(base / name))
        throw std::runtime_error("invalid owned cleanup directory");
    // canonical() drops the extended-length prefix, so its results are compared in long form.
    const auto resolved_parent = long_path(fs::canonical(base)), resolved = long_path(fs::canonical(owned));
    if (resolved_parent != base || resolved != long_path(resolved_parent / name))
        throw std::runtime_error("owned cleanup directory or parent is redirected");
    for (const auto& entry : fs::recursive_directory_iterator(owned)) {
        const auto relative = entry.path().lexically_relative(owned);
        if (entry.is_symlink() || long_path(fs::canonical(entry.path())) != long_path(resolved / relative))
            throw std::runtime_error("owned cleanup directory contains a redirected path");
    }
    fs::remove_all(resolved);
}

}  // namespace spirula
