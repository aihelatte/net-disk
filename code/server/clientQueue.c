#include"threadPool.h"
#include "worker.h"

//初始化客户队列
int init_clientQueue(clientQueue_t *pclientQueue)
{
    pclientQueue->timer = 0;
    //初始化客户端数组的usr_id
    memset(pclientQueue->client, -1, sizeof(pclientQueue->client));
    //初始化哈希表，内容为netfd对于所在循环队列的位置
    memset(pclientQueue->index, 0, sizeof(pclientQueue->index));
    for(int i = 0; i < TIME_SLICE; ++i)//初始化循环队列里面的集合（队列） 
    {
        pclientQueue->time_out[i].head = NULL;
        pclientQueue->time_out[i].tail = NULL;
        pclientQueue->time_out[i].size = 0;
    }
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
    slotNode_t *pre = cur;
    if(cur == NULL)// 可能已经为空
    {
        return 0;
    }
    if(cur->netFd == netFd)     //开头第一个的情况
    {
        pclientQueue->time_out[index].head = cur->next;
        free(cur);
        --pclientQueue->time_out[index].size;
    }
    else
    {
        while(cur != NULL)
        {
            if (cur->netFd == netFd)
            {
                pre->next = cur->next;
                free(cur);
                --pclientQueue->time_out[index].size;
                break;
            }
            pre = cur;
            cur = cur->next;
        }
    }
}
