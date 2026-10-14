#pragma once

// A remote session's file routes: this machine's files to the page and the
// page's to this machine, and the web viewer the page opens splats in. Every
// route is behind the session's token. docs/notes/file-browser.md.

#include <cstdint>
#include <functional>
#include <string>

class HttpServer;

namespace gui {
namespace remote {

// /viewer/, /remote/raw/, /remote/download/ and /remote/upload, as the route
// table in docs/notes/file-browser.md has them. `on_upload` runs after each
// file an upload completes.
void add_file_routes(HttpServer& http, std::function<void()> on_upload);

// The Content-Type a file is served with, by its extension.
const char* mime_type(const std::string& name);

// A Range header ("bytes=a-b", "bytes=a-", "bytes=-n") against a file of
// `size` bytes, as [first, last]. False when absent or unsatisfiable.
bool parse_range(const std::string& header, uint64_t size, uint64_t& first, uint64_t& last);

}  // namespace remote
}  // namespace gui
