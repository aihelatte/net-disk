#include "threadPool.h"
#include "worker.h"

// 主线程的回复先入缓冲区，由EPOLLOUT继续发送，不调用等待式sendn
int connection_t::queueReply(const void *buf, int length)
{
    if(length < 0 || (size_t)length > MAX_CONTROL_REPLY - sendBuf.size())
    {
        errno = EMSGSIZE;
        return -1;
    }
    if(length == 0)
    {
        return 0;
    }
    const char *bytes = static_cast<const char *>(buf);
    try {
        sendBuf.insert(sendBuf.end(), bytes, bytes + length);
    } catch(const std::bad_alloc &) {
        return -1;
    }
    return length;
}

int connection_t::flushReply()
{
    int total = 0;
    // 单次事件最多发送64 KiB，给其他连接和时间轮保留处理机会
    while(sendOffset < sendBuf.size() && total < 64 * 1024)
    {
        size_t length = sendBuf.size() - sendOffset;
        if(length > (size_t)(64 * 1024 - total))
        {
            length = 64 * 1024 - total;
        }
        ssize_t ret = send(netFd,
                           sendBuf.data() + sendOffset,
                           length, MSG_NOSIGNAL | MSG_DONTWAIT);
        if(ret > 0)
        {
            sendOffset += ret;
            total += ret;
        }
        else if(ret == -1 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        {
            return total;
        }
        else
        {
            return -1;
        }
    }
    if(sendOffset == sendBuf.size())
    {
        std::vector<char>().swap(sendBuf);
        sendOffset = 0;
    }
    return total;
}
