#include"threadPool.h"
#include "worker.h"
#include <sys/resource.h>
#include <algorithm>

//初始化客户队列
int init_clientQueue(clientQueue_t *pclientQueue)
{
    struct rlimit fdLimit;
    if(getrlimit(RLIMIT_NOFILE, &fdLimit) == -1)
    {
        perror("getrlimit");
        return -1;
    }
    if(fdLimit.rlim_cur == RLIM_INFINITY || fdLimit.rlim_cur == 0 ||
       fdLimit.rlim_cur > SIZE_MAX / sizeof(int) ||
       fdLimit.rlim_cur > SIZE_MAX / sizeof(connection_t *))
    {
        fprintf(stderr, "Invalid file descriptor limit.\n");
        return -1;
    }

    pclientQueue->fd_capacity = (size_t)fdLimit.rlim_cur;
    try {
        pclientQueue->client.assign(pclientQueue->fd_capacity, -1);
        pclientQueue->index.assign(pclientQueue->fd_capacity, -1);
        pclientQueue->connections.resize(pclientQueue->fd_capacity);
    } catch(const std::exception &) {
        return -1;
    }
    pclientQueue->timer = 0;
    return 0;
}

// 先插入新槽，分配失败时仍保留原有的超时位置。
int fdAdd(int netFd, clientQueue_t *pclientQueue)
{
    if(netFd < 0 || static_cast<size_t>(netFd) >= pclientQueue->fd_capacity)
        return -1;
    int slot = (pclientQueue->timer + TIME_SLICE - 1) % TIME_SLICE;
    if(pclientQueue->index[netFd] == slot)
        return 0;
    try {
        pclientQueue->time_out[slot].push_back(netFd);
    } catch(const std::bad_alloc &) {
        return -1;
    }
    fdDel(netFd, pclientQueue);
    pclientQueue->index[netFd] = slot;
    return 0;
}

int fdDel(int netFd, clientQueue_t *pclientQueue)
{
    if(netFd < 0 || static_cast<size_t>(netFd) >= pclientQueue->fd_capacity)
        return -1;
    int slot = pclientQueue->index[netFd];
    if(slot >= 0) {
        auto &entries = pclientQueue->time_out[slot];
        auto entry = std::find(entries.begin(), entries.end(), netFd);
        if(entry != entries.end())
            entries.erase(entry);
        pclientQueue->index[netFd] = -1;
    }
    return 0;
}
