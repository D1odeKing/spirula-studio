#pragma once

// A folder as a tar stream, written as it is read: a download of a 40 GB
// dataset never stages a copy on disk or in memory. ustar, with GNU long-name
// records for paths over 100 bytes, which every tar since the 1990s reads.

#include <cstdint>
#include <functional>
#include <string>

namespace gui {

// Returns false to stop (the reader went away).
using TarSink = std::function<bool(const void* data, size_t len)>;

// `root` and everything under it, named from `root`'s own name down; symlinks
// are followed for files, not for folders. False when `sink` stopped it;
// unreadable files are skipped and counted in `skipped`.
bool write_tar(const std::string& root, const TarSink& sink, int* skipped = nullptr);

}  // namespace gui
