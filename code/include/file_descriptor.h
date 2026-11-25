#pragma once
#include <unistd.h>

class FileDescriptor {
public:
    explicit FileDescriptor(int fd) : fd_(fd) {}
    ~FileDescriptor() { reset(); }
    void reset() {
        if(fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }
    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;
    int get() const { return fd_; }
private:
    int fd_;
};
