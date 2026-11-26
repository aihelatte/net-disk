#include "threadPool.h"
#include <utility>

// 在调用方持锁时入队一个数据连接。成功返回 0，失败时 fd 仍由调用方持有。
int TaskQueue::push(int code, FileDescriptor &netFd)
{
    int64_t now = network_time_ms();
    if (now < 0)
    {
        return -1;
    }
    try
    {
        tasks.push({code, FileDescriptor{}, now});
    } catch(const std::bad_alloc &) {
        return -1;
    }
    // 队列位置已分配成功，现在才能接管套接字。
    tasks.back().netFd = std::move(netFd);
    return 0;
}

// 在队列锁内丢弃等待超时的任务，任务析构时关闭仍持有的数据连接。
void TaskQueue::expire(int64_t now)
{
    while (!empty() && now - front().queuedAt >= NETWORK_IDLE_TIMEOUT_MS)
    {
        pop();
    }
}
