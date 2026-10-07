#pragma once

#include <filesystem>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace spirula {

class FileLock {
public:
    explicit FileLock(const std::filesystem::path& file) {
#ifdef _WIN32
        handle_ = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            throw std::runtime_error("cannot acquire exclusive run lock: " + file.string());
#else
        handle_ = ::open(file.c_str(), O_CREAT | O_RDWR, 0666);
        if (handle_ < 0) throw std::runtime_error("cannot open run lock: " + file.string());
        if (flock(handle_, LOCK_EX | LOCK_NB) != 0) {
            ::close(handle_); handle_ = -1;
            throw std::runtime_error("cannot acquire exclusive run lock: " + file.string());
        }
#endif
    }
    ~FileLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
        if (handle_ >= 0) ::close(handle_);
#endif
    }
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int handle_ = -1;
#endif
};

}  // namespace spirula
