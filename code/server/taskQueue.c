#include "threadPool.h"
#include "worker.h"
int taskEnqueue(taskQueue_t *pTaskQueue, int code, int netFd)
{
    task_t *pTask = (task_t *)calloc(1, sizeof(task_t));
    if(pTask == NULL)
    {
        perror("calloc");
        return EXIT_FAILURE;
    }
    pTask->queuedAt = network_time_ms();
    if(pTask->queuedAt < 0)
    {
        free(pTask);
        return -1;
    }
    pTask->code = code;
    pTask->netFd = netFd;
    if(pTaskQueue->size == 0)
    {
        pTaskQueue->pFront = pTask;
        pTaskQueue->pRear = pTask;
    }
    else
    {
        pTaskQueue->pRear->pNext = pTask;
        pTaskQueue->pRear = pTask;
    }
    ++pTaskQueue->size;
    return 0;
}

int taskDequeue(taskQueue_t *pTaskQueue)
{
    if(pTaskQueue->pFront == NULL)
    {
        printf("Is empty!\n");
        return 0;
    }
    task_t *pCur = pTaskQueue->pFront;
    pTaskQueue->pFront = pCur->pNext;
    free(pCur);
    --pTaskQueue->size;
    if(pTaskQueue->size == 0)
    {
        pTaskQueue->pRear = NULL;
    }
    return 0;
}

// 调用方持有队列锁；队列按入队时间排列，只清理尚未交给工作线程的任务
void taskQueueExpire(taskQueue_t *pTaskQueue, int64_t now)
{
    while(pTaskQueue->pFront != NULL &&
          now - pTaskQueue->pFront->queuedAt >= NETWORK_IDLE_TIMEOUT_MS)
    {
        int netFd = pTaskQueue->pFront->netFd;
        taskDequeue(pTaskQueue);
        close(netFd);
    }
}
