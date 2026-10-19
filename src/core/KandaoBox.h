#pragma once
// The two top-level boxes a Kandao camera (QooCam) ends its MP4 with: `kfix`,
// what holds for the whole recording, and `kvar`, its logs. Both are a
// little-endian u32 field count, then per field name[32], type[8], a u32
// element count and the elements; a log is one U8 field of records. Read by
// app/Pano360.cpp and sfm/core/Telemetry.cpp.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace kandao {

struct Field {
    std::string name, type;
    const uint8_t* data = nullptr;
    size_t size = 0;   // bytes
    std::string str() const {
        const char* c = (const char*)data;
        return std::string(c, strnlen(c, size));
    }
};

// Bytes per element; 0 for a type outside the twelve ExifTool lists.
inline size_t type_width(const std::string& t) {
    if (t == "CHAR" || t == "BOOL" || t == "U8" || t == "S8") return 1;
    if (t == "U16" || t == "S16") return 2;
    if (t == "U32" || t == "S32" || t == "FLOAT") return 4;
    if (t == "U64" || t == "S64" || t == "DOUBLE") return 8;
    return 0;
}

// In order. A type it does not know ends the list, since nothing gives that
// field's size; everything before it is still returned.
inline std::vector<Field> fields(const uint8_t* p, size_t n) {
    std::vector<Field> out;
    if (!p || n < 4) return out;
    const uint32_t count = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
                           (uint32_t)p[3] << 24;
    size_t o = 4;
    for (uint32_t i = 0; i < count && o + 44 <= n; i++) {
        Field f;
        f.name.assign((const char*)p + o, strnlen((const char*)p + o, 32));
        f.type.assign((const char*)p + o + 32, strnlen((const char*)p + o + 32, 8));
        const uint8_t* c = p + o + 40;
        const uint64_t elements = (uint64_t)c[0] | (uint64_t)c[1] << 8 | (uint64_t)c[2] << 16 |
                                  (uint64_t)c[3] << 24;
        const uint64_t size = elements * type_width(f.type);
        o += 44;
        if (type_width(f.type) == 0 || size > n - o) break;
        f.data = p + o;
        f.size = (size_t)size;
        o += f.size;
        out.push_back(std::move(f));
    }
    return out;
}

inline const Field* find(const std::vector<Field>& fs, const char* name) {
    for (const Field& f : fs)
        if (f.name == name) return &f;
    return nullptr;
}

// A one-byte field's value (STITCHED, STEREO); -1 when absent.
inline int byte_value(const std::vector<Field>& fs, const char* name) {
    const Field* f = find(fs, name);
    return f && f->size >= 1 ? f->data[0] : -1;
}

// One `KEY=value` of kvar's space-separated INFO string; < 0 when absent.
inline double info_value(const std::string& info, const char* key) {
    const std::string k = std::string(key) + "=";
    size_t at = 0;
    while ((at = info.find(k, at)) != std::string::npos) {
        if (at == 0 || info[at - 1] == ' ') return std::atof(info.c_str() + at + k.size());
        at += k.size();
    }
    return -1;
}

}  // namespace kandao
