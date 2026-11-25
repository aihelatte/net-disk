#include "threadPool.h"

int TaskQueue::push(int code, int netFd)
{
    int64_t now = network_time_ms();
    if(now < 0)
        return -1;
    try {
        tasks.push({code, netFd, now});
    } catch(const std::bad_alloc &) {
        return -1;
    }
    return 0;
}

void TaskQueue::expire(int64_t now)
{
    while(!empty() && now - front().queuedAt >= NETWORK_IDLE_TIMEOUT_MS)
    {
        close(front().netFd);
        pop();
    }
}
