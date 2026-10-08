#pragma once
// The wording scan the licence-text tests share: a summary or title a user reads
// before accepting must carry no restriction the licence does not.

#include <cctype>
#include <string>

namespace licwording {

inline std::string lower(std::string s) {
    for (char& c : s)
        if ((unsigned char)c < 0x80) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Stems, so inflected forms are caught; each language's words for "commercial",
// "non-commercial" and "redistribute" (and the same ideas in the nearest noun).
inline const char* const kBanned[] = {
    "commercial", "redistribut", "re-distribut", "sublicens", "resell", "resale",
    "kommerz", "weiterverbreit", "weitergabe", "weiterverkauf",
    "commercia", "redistribu", "revend",
    "comercial", "redistribu", "reventa", "revenda",
    "commerci", "herdistribu", "doorverkoop",
    "коммерч", "перераспростран", "перепрод",
    "ticari", "yeniden dağıt", "yeniden sat",
    "商用", "商业", "商業", "非営利", "営利", "再配布", "再頒布", "転売",
    "非商业", "非商業", "再分发", "再分發", "转售", "轉售", "再发布", "再發布",
    "상업", "영리", "재배포", "재판매",
};


// The first banned stem in `text` (compared lower-cased), or "" when it has none.
inline std::string banned_in(const std::string& text) {
    const std::string t = lower(text);
    for (const char* w : kBanned)
        if (t.find(lower(w)) != std::string::npos) return w;
    return "";
}

}  // namespace licwording
