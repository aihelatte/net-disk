#include"threadPool.h"
#include "worker.h"
#include <sys/resource.h>

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
    pclientQueue->client = calloc(pclientQueue->fd_capacity, sizeof(int));
    pclientQueue->index = calloc(pclientQueue->fd_capacity, sizeof(int));
    pclientQueue->connections = calloc(pclientQueue->fd_capacity, sizeof(connection_t *));
    if(pclientQueue->client == NULL || pclientQueue->index == NULL ||
       pclientQueue->connections == NULL)
    {
        free(pclientQueue->client);
        free(pclientQueue->index);
        free(pclientQueue->connections);
        return -1;
    }

    pclientQueue->timer = 0;
    //初始化客户端数组的usr_id
    memset(pclientQueue->client, -1, pclientQueue->fd_capacity * sizeof(int));
    //初始化哈希表，内容为netfd对于所在循环队列的位置
    memset(pclientQueue->index, -1, pclientQueue->fd_capacity * sizeof(int));
    for(int i = 0; i < TIME_SLICE; ++i)//初始化循环队列里面的集合（队列） 
    {
        pclientQueue->time_out[i].head = NULL;
        pclientQueue->time_out[i].tail = NULL;
        pclientQueue->time_out[i].size = 0;
    }
    return 0;
}

//事件加入集合
int fdAdd(int netFd, clientQueue_t *pclientQueue)// 尾插法
{
    if(netFd < 0 || (size_t)netFd >= pclientQueue->fd_capacity)
    {
        return -1;
    }
    slotNode_t *newSlot = (slotNode_t *)calloc(1, sizeof(slotNode_t));
    if(newSlot == NULL)
    {
        return -1;
    }
    fdDel(netFd, pclientQueue);
    newSlot->netFd = netFd;
    pclientQueue->index[netFd] = (pclientQueue->timer + TIME_SLICE - 1) % TIME_SLICE;// 加入时有TIME_SLICE的时间，timer是下一次要检查的地方
    int index = (pclientQueue->timer + TIME_SLICE - 1) % TIME_SLICE;
    //集合大小加一
    ++pclientQueue->time_out[index].size;
    if(pclientQueue->time_out[index].head == NULL)
    {
        pclientQueue->time_out[index].head = newSlot;
        pclientQueue->time_out[index].tail = newSlot;
    }
    else
    {
        pclientQueue->time_out[index].tail->next = newSlot;
        pclientQueue->time_out[index].tail = newSlot;
    }
    return 0;
}

//在时间转轮中删除netfd
int fdDel(int netFd, clientQueue_t *pclientQueue)
{
    //利用哈希根据netfd获取所在下标
    if(netFd < 0 || (size_t)netFd >= pclientQueue->fd_capacity)
    {
        return -1;
    }
    int index = pclientQueue->index[netFd];
    if(index < 0 || index >= TIME_SLICE)
    {
        return 0;
    }
    pclientQueue->index[netFd] = -1;// 标记已从时间轮移除
    slotNode_t *cur = pclientQueue->time_out[index].head;
    slotNode_t *pre = NULL;
    while(cur != NULL)
    {
        if(cur->netFd == netFd)
        {
            if(pre == NULL)// 删除头节点
            {
                pclientQueue->time_out[index].head = cur->next;
            }
            else// 删除中间节点或尾节点
            {
                pre->next = cur->next;
            }

            if(pclientQueue->time_out[index].tail == cur)// 删除尾节点
            {
                pclientQueue->time_out[index].tail = pre;
            }

            free(cur);
            --pclientQueue->time_out[index].size;
            return 0;
        }
        pre = cur;
        cur = cur->next;
    }
    return 0;
}
