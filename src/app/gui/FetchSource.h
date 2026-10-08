#pragma once
// A nn::FetchFile as the GUI's download queue takes it. The one place the GUI
// turns a FetchFile into a PendingDownload, so its mirror and its licence are
// resolved exactly as nn::ensure_file resolves them (nn::mirror_url).

#include "app/gui/ModelCache.h"
#include "nn/io/Fetch.h"

namespace gui {

inline PendingDownload pending_download(const nn::FetchFile& f) {
    return {f.url, nn::cached_path(f), f.bytes, nn::mirror_url(f),
            f.license_family ? f.license_family : ""};
}

}  // namespace gui
