#ifndef NET_DISK_NET_IO_H
#define NET_DISK_NET_IO_H

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <time.h>

#define NETWORK_IDLE_TIMEOUT_MS 30000

static inline int socket_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        return -1;
    }
    return 0;
}

static inline int64_t network_time_ms(void)
{
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC, &now) == -1)
    {
        return -1;
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

// 只在实际收发到字节时更新lastProgress，信号和就绪通知不重置计时
static inline int network_remaining_ms(int64_t lastProgress)
{
    int64_t now = network_time_ms();
    if(now < 0)
    {
        return -1;
    }
    int64_t remaining = NETWORK_IDLE_TIMEOUT_MS - (now - lastProgress);
    if(remaining <= 0)
    {
        errno = ETIMEDOUT;
        return -1;
    }
    return (int)remaining;
}

static inline int wait_socket(int fd, short events, int64_t lastProgress)
{
    struct pollfd ready;
    ready.fd = fd;
    ready.events = events;
    while(1)
    {
        int remaining = network_remaining_ms(lastProgress);
        if(remaining < 0)
        {
            return -1;
        }
        ready.revents = 0;
        int ret = poll(&ready, 1, remaining);
        if(ret > 0)
        {
            if(ready.revents & POLLNVAL)
            {
                errno = EBADF;
                return -1;
            }
            if(ready.revents & (events | POLLERR | POLLHUP))
            {
                return 0;// 由下一次收发识别断开或具体错误
            }
        }
        else if(ret == 0)
        {
            errno = ETIMEDOUT;
            return -1;
        }
        else if(errno != EINTR)
        {
            return -1;
        }
    }
}

static inline int network_recvn(int fd, void *buf, int length)
{
    if(length < 0)
    {
        errno = EINVAL;
        return -1;
    }
    int total = 0;
    int64_t lastProgress = network_time_ms();
    if(lastProgress < 0)
    {
        return -1;
    }
    while(total < length)
    {
        if(network_remaining_ms(lastProgress) < 0)
        {
            return -1;
        }
        ssize_t ret = recv(fd, (char *)buf + total, length - total, MSG_DONTWAIT);
        if(ret > 0)
        {
            total += ret;
            lastProgress = network_time_ms();
            if(lastProgress < 0)
            {
                return -1;
            }
        }
        else if(ret == 0)
        {
            return total;
        }
        else if(errno == EAGAIN || errno == EWOULDBLOCK)
        {
            if(wait_socket(fd, POLLIN, lastProgress) != 0)
            {
                return -1;
            }
        }
        else if(errno != EINTR)
        {
            return -1;
        }
    }
    return total;
}

static inline int network_sendn(int fd, const void *buf, int length, int flags)
{
    if(length < 0)
    {
        errno = EINVAL;
        return -1;
    }
    int total = 0;
    int64_t lastProgress = network_time_ms();
    if(lastProgress < 0)
    {
        return -1;
    }
    while(total < length)
    {
        if(network_remaining_ms(lastProgress) < 0)
        {
            return -1;
        }
        ssize_t ret = send(fd, (const char *)buf + total, length - total,
                           flags | MSG_DONTWAIT | MSG_NOSIGNAL);
        if(ret > 0)
        {
            total += ret;
            lastProgress = network_time_ms();
            if(lastProgress < 0)
            {
                return -1;
            }
        }
        else if(ret == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            if(wait_socket(fd, POLLOUT, lastProgress) != 0)
            {
                return -1;
            }
        }
        else if(ret == 0 || errno != EINTR)
        {
            return ret;
        }
    }
    return total;
}

// 调用方先把socket设为非阻塞；offset仍由sendfile按实际发送量更新
static inline ssize_t network_sendfile(int fd, int fileFd, off_t *offset, size_t length)
{
    int64_t lastProgress = network_time_ms();
    if(lastProgress < 0)
    {
        return -1;
    }
    while(1)
    {
        if(network_remaining_ms(lastProgress) < 0)
        {
            return -1;
        }
        ssize_t ret = sendfile(fd, fileFd, offset, length);
        if(ret >= 0)
        {
            return ret;
        }
        if(errno == EAGAIN || errno == EWOULDBLOCK)
        {
            if(wait_socket(fd, POLLOUT, lastProgress) != 0)
            {
                return -1;
            }
        }
        else if(errno != EINTR)
        {
            return -1;
        }
    }
}

#endif
