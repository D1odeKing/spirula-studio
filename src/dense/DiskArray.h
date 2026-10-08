#pragma once

#include "dense/ExternalSort.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace spirula::dense {

template<class T> class DiskArray {
    static_assert(std::is_trivially_copyable_v<T>, "disk records must be trivially copyable");
public:
    DiskArray(std::filesystem::path file, uint64_t budget)
        : file_(std::move(file)), ordered_(file_.string() + ".ordered"), limit_(std::max<uint64_t>(1, budget / sizeof(T))) {}
    ~DiskArray() { clear(); }
    DiskArray(const DiskArray&) = delete;
    DiskArray& operator=(const DiskArray&) = delete;

    uint64_t size() const { return size_; }
    bool empty() const { return !size_; }
    T front() const { return at(0); }

    void push_back(const T& value) {
        if (!stream_.is_open() && values_.size() == limit_) {
            stream_.open(file_, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
            if (!stream_) throw std::runtime_error("cannot open dense spill file");
            spilled_ = true;
            stream_.write(reinterpret_cast<const char*>(values_.data()), (std::streamsize)(values_.size() * sizeof(T)));
            std::vector<T>().swap(values_);
        }
        if (stream_.is_open()) {
            stream_.clear(); stream_.seekp(0, std::ios::end);
            stream_.write(reinterpret_cast<const char*>(&value), sizeof(T));
            if (!stream_) throw std::runtime_error("cannot write dense spill file");
            dirty_ = true;
        } else {
            if (values_.size() == values_.capacity())
                values_.reserve((size_t)std::min<uint64_t>(limit_, std::max<uint64_t>(1, values_.capacity() * 2)));
            values_.push_back(value);
        }
        ++size_;
    }

    T at(uint64_t index) const {
        if (index >= size_) throw std::out_of_range("dense spill index");
        if (!stream_.is_open()) return values_[(size_t)index];
        if (dirty_) {
            stream_.flush(); if (!stream_) throw std::runtime_error("cannot flush dense spill file"); dirty_ = false;
        }
        stream_.clear(); stream_.seekg((std::streamoff)(index * sizeof(T)));
        T value;
        stream_.read(reinterpret_cast<char*>(&value), sizeof(T));
        if (!stream_) throw std::runtime_error("cannot read dense spill file");
        return value;
    }
    T operator[](uint64_t index) const { return at(index); }

    void set(uint64_t index, const T& value) {
        if (index >= size_) throw std::out_of_range("dense spill index");
        if (!stream_.is_open()) { values_[(size_t)index] = value; return; }
        stream_.clear(); stream_.seekp((std::streamoff)(index * sizeof(T)));
        stream_.write(reinterpret_cast<const char*>(&value),sizeof(T));
        if (!stream_) throw std::runtime_error("cannot update dense spill file");
        dirty_ = true;
    }

    void resize(uint64_t count) {
        if (count > size_) throw std::out_of_range("dense spill resize cannot add records");
        if (!count) { clear(); return; }
        if (stream_.is_open()) {
            stream_.flush(); if (!stream_) throw std::runtime_error("cannot flush dense spill file");
            stream_.close(); std::filesystem::resize_file(file_,count * sizeof(T));
            stream_.open(file_,std::ios::binary | std::ios::in | std::ios::out);
            if (!stream_) throw std::runtime_error("cannot reopen dense spill file");
            dirty_ = false;
        } else values_.resize((size_t)count);
        size_ = count;
    }

    template<class Compare> void sort(Compare compare, uint64_t budget, const std::function<void()>& check = {}) {
        if (!stream_.is_open()) { std::stable_sort(values_.begin(),values_.end(),compare); return; }
        stream_.flush(); if (!stream_) throw std::runtime_error("cannot flush dense spill file");
        stream_.close(); dirty_ = false;
        external_sort<T>(file_,ordered_,budget,compare,check);
        std::filesystem::remove(file_); std::filesystem::rename(ordered_,file_);
        stream_.open(file_,std::ios::binary | std::ios::in | std::ios::out);
        if (!stream_) throw std::runtime_error("cannot reopen sorted dense spill file");
    }

    void clear() noexcept {
        if (stream_.is_open()) stream_.close();
        if (spilled_) {
            std::error_code error; std::filesystem::remove(file_, error);
            std::filesystem::remove(ordered_, error);
        }
        values_.clear(); size_ = 0; dirty_ = false; spilled_ = false;
    }

    struct Iterator {
        const DiskArray* array;
        uint64_t index;
        T operator*() const { return array->at(index); }
        Iterator& operator++() { ++index; return *this; }
        bool operator!=(const Iterator& other) const { return index != other.index; }
    };
    Iterator begin() const { return {this, 0}; }
    Iterator end() const { return {this, size_}; }

private:
    std::filesystem::path file_, ordered_;
    uint64_t limit_, size_ = 0;
    std::vector<T> values_;
    bool spilled_ = false;
    mutable std::fstream stream_;
    mutable bool dirty_ = false;
};

}  // namespace spirula::dense
