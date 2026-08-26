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
    if(fdLimit.rlim_cur == RLIM_INFINITY || fdLimit.rlim_cur == 0)
    {
        fprintf(stderr, "Invalid file descriptor limit.\n");
        return -1;
    }

    pclientQueue->fd_capacity = (size_t)fdLimit.rlim_cur;
    pclientQueue->client = calloc(pclientQueue->fd_capacity, sizeof(int));
    pclientQueue->index = calloc(pclientQueue->fd_capacity, sizeof(int));
    if(pclientQueue->client == NULL || pclientQueue->index == NULL)
    {
        free(pclientQueue->client);
        free(pclientQueue->index);
        return -1;
    }

    pclientQueue->timer = 0;
    //初始化客户端数组的usr_id
    memset(pclientQueue->client, -1, pclientQueue->fd_capacity * sizeof(int));
    //初始化哈希表，内容为netfd对于所在循环队列的位置
    memset(pclientQueue->index, 0, pclientQueue->fd_capacity * sizeof(int));
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
    slotNode_t *newSlot = (slotNode_t *)calloc(1, sizeof(slotNode_t));
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
}

//在时间转轮中删除netfd
int fdDel(int netFd, clientQueue_t *pclientQueue)
{
    //利用哈希根据netfd获取所在下标
    int index = pclientQueue->index[netFd];
    pclientQueue->index[netFd] = 0;// 重设为0
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
