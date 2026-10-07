#pragma once

#include "dense/ExternalSort.h"

#include <cmath>
#include <list>
#include <map>
#include <type_traits>

namespace spirula::dense {

template<class T> class DiskTable {
    static_assert(std::is_trivially_copyable_v<T>, "disk records must be trivially copyable");
    struct Page {
        std::vector<T> records;
        std::list<uint64_t>::iterator age;
        bool dirty = false;
    };
public:
    DiskTable(const std::filesystem::path& path, uint64_t budget, bool writable = false) : writable_(writable) {
        const auto bytes = std::filesystem::file_size(path);
        if (bytes % sizeof(T)) throw std::runtime_error("truncated dense table");
        size_ = bytes / sizeof(T);
        records_per_page_ = std::max<uint64_t>(1, (uint64_t)std::sqrt((double)(budget / sizeof(T))));
        records_per_page_ = std::min(records_per_page_,std::max<uint64_t>(1,size_));
        const uint64_t page_bytes = records_per_page_ * sizeof(T) + sizeof(Page) + 7 * sizeof(void*) + sizeof(uint64_t);
        page_limit_ = std::max<uint64_t>(1,budget / page_bytes);
        stream_.open(path,std::ios::binary | std::ios::in | (writable ? std::ios::out : std::ios::openmode{}));
        if (!stream_) throw std::runtime_error("cannot open dense table");
    }
    ~DiskTable() { try { flush(); } catch (const std::exception&) {} }
    DiskTable(const DiskTable&) = delete;
    DiskTable& operator=(const DiskTable&) = delete;

    uint64_t size() const { return size_; }
    T get(uint64_t index) {
        auto& value = page(index);
        return value.records[(size_t)(index % records_per_page_)];
    }
    void set(uint64_t index, const T& value) {
        if (!writable_) throw std::runtime_error("dense table is read-only");
        auto& selected = page(index);
        selected.records[(size_t)(index % records_per_page_)] = value;
        selected.dirty = true;
    }
    void flush() {
        for (auto& [index,value] : pages_) write(index,value);
        if (writable_) { stream_.flush(); if (!stream_) throw std::runtime_error("cannot flush dense table"); }
    }
private:
    void write(uint64_t index, Page& value) {
        if (!value.dirty) return;
        stream_.clear(); stream_.seekp((std::streamoff)(index * records_per_page_ * sizeof(T)));
        stream_.write(reinterpret_cast<const char*>(value.records.data()),(std::streamsize)(value.records.size() * sizeof(T)));
        if (!stream_) throw std::runtime_error("cannot write dense table");
        value.dirty = false;
    }
    Page& page(uint64_t index) {
        if (index >= size_) throw std::out_of_range("dense table index");
        const auto number = index / records_per_page_;
        auto found = pages_.find(number);
        if (found != pages_.end()) {
            ages_.splice(ages_.begin(),ages_,found->second.age); return found->second;
        }
        if (pages_.size() >= page_limit_) {
            const auto oldest = ages_.back(); auto entry = pages_.find(oldest);
            write(oldest,entry->second); pages_.erase(entry); ages_.pop_back();
        }
        Page value;
        value.records.resize((size_t)std::min(records_per_page_,size_ - number * records_per_page_));
        stream_.clear(); stream_.seekg((std::streamoff)(number * records_per_page_ * sizeof(T)));
        stream_.read(reinterpret_cast<char*>(value.records.data()),(std::streamsize)(value.records.size() * sizeof(T)));
        if (!stream_) throw std::runtime_error("cannot read dense table");
        ages_.push_front(number); value.age = ages_.begin();
        return pages_.emplace(number,std::move(value)).first->second;
    }
    std::fstream stream_;
    bool writable_;
    uint64_t size_, records_per_page_, page_limit_;
    std::map<uint64_t,Page> pages_;
    std::list<uint64_t> ages_;
};

}  // namespace spirula::dense
