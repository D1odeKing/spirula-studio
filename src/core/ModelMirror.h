#pragma once

// The fallback host for every checkpoint the program downloads. Hugging Face
// and GitHub releases are unreachable from Mainland China; this ModelScope
// repository re-hosts the same files, flat, under their model-cache names.
// A file is only fetched from here after the upstream URL fails.

#include <string>

namespace spirula {

inline std::string model_mirror_url(const std::string& cache_name) {
    return "https://modelscope.cn/models/wbbaaoo/spirula/resolve/master/" + cache_name;
}

// The fallback URL: the model's own mirror, else the project's. Every download
// path resolves its fallback here.
inline std::string mirror_for(const std::string& cache_name, const char* own_mirror) {
    return own_mirror ? std::string(own_mirror) : model_mirror_url(cache_name);
}

}  // namespace spirula
