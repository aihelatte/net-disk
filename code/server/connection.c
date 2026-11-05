#include "threadPool.h"
#include "worker.h"

// 主线程的回复先入缓冲区，由EPOLLOUT继续发送，不调用等待式sendn
int queue_reply(connection_t *pConnection, const void *buf, int length)
{
    if(length < 0 || (size_t)length > MAX_CONTROL_REPLY - pConnection->sendSize)
    {
        errno = EMSGSIZE;
        return -1;
    }
    if(length == 0)
    {
        return 0;
    }
    char *newBuf = realloc(pConnection->sendBuf, pConnection->sendSize + length);
    if(newBuf == NULL)
    {
        return -1;
    }
    pConnection->sendBuf = newBuf;
    memcpy(pConnection->sendBuf + pConnection->sendSize, buf, length);
    pConnection->sendSize += length;
    return length;
}

int flush_reply(connection_t *pConnection)
{
    int total = 0;
    // 单次事件最多发送64 KiB，给其他连接和时间轮保留处理机会
    while(pConnection->sendOffset < pConnection->sendSize && total < 64 * 1024)
    {
        size_t length = pConnection->sendSize - pConnection->sendOffset;
        if(length > (size_t)(64 * 1024 - total))
        {
            length = 64 * 1024 - total;
        }
        ssize_t ret = send(pConnection->netFd,
                           pConnection->sendBuf + pConnection->sendOffset,
                           length, MSG_NOSIGNAL | MSG_DONTWAIT);
        if(ret > 0)
        {
            pConnection->sendOffset += ret;
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
    if(pConnection->sendOffset == pConnection->sendSize)
    {
        free(pConnection->sendBuf);
        pConnection->sendBuf = NULL;
        pConnection->sendSize = 0;
        pConnection->sendOffset = 0;
    }
    return total;
}
