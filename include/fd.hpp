#pragma once
#include <unistd.h>

class Fd {
public:
    Fd() noexcept = default;

    explicit Fd(int fd) noexcept : fd_(fd) {}

    ~Fd()
    {
        if (fd_ >= 0)
        {
            close(fd_);
        }
    }

    // No copying: unique ownership.
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    // Move constructor: steal other's fd, then invalidate the source so its
    // destructor won't close the fd we just took.
    Fd(Fd&& other) noexcept : fd_(other.fd_)
    {
        other.fd_ = -1;
    }

    // Move assignment: release what we currently own, then steal other's.
    Fd& operator=(Fd&& other) noexcept
    {
        if (this != &other)
        {
            if (fd_ >= 0) close(fd_);
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    // Raw fd for syscalls (read/write/epoll_ctl/...). Borrowing, not owning:
    // don't close() the returned value — the Fd still owns it.
    int get() const noexcept { return fd_; }

    bool valid() const noexcept { return fd_ >= 0; }

    // Give up ownership WITHOUT closing; caller becomes responsible for the fd.
    int release() noexcept {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }

private:
    int fd_ = -1;   // -1 = invalid sentinel
};
