#pragma once
#include <unistd.h>

// 独占一个 Linux 文件描述符。析构负责关闭，移动用于显式移交所有权。
class FileDescriptor
{
public:
    FileDescriptor() noexcept = default;
    explicit FileDescriptor(int fd) noexcept : fd_(fd) {}
    ~FileDescriptor() { reset(); }
    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;
    // release 取出 fd，并将源对象置为空，避免源对象析构时重复关闭。
    FileDescriptor(FileDescriptor &&other) noexcept : fd_(other.release()) {}
    FileDescriptor &operator=(FileDescriptor &&other) noexcept
    {
        if (this != &other)
        {
            // 先关闭当前对象持有的旧 fd，再接管源对象交出的 fd。
            reset(other.release());
        }
        return *this;
    }
    int get() const noexcept { return fd_; }
    // 移交 fd 给调用方。此后关闭它是调用方的责任。
    int release() noexcept
    {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }
    void reset(int fd = -1) noexcept
    {
        if (fd_ == fd)
        {
            return;
        }
        if (fd_ >= 0)
        {
            close(fd_);
        }
        fd_ = fd;
    }
private:
    int fd_ = -1;
};
