#include "threadPool.h"
#include "worker.h"
int taskEnqueue(taskQueue_t *pTaskQueue, server_t *pserver)
{
    task_t *pTask = (task_t *)calloc(1, sizeof(task_t));
    if(pTask == NULL)
    {
        perror("calloc");
        return EXIT_FAILURE;
    }
    pTask->server = *pserver;
    // printf("check!\n"); 
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
