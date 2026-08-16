#ifndef _THREADPOOL_H// 头文件在编译时被多次引用，避免重复定义
#define _THREADPOOL_H
#include<myhead.h>
#include "worker.h"

//一个任务单元结构体
typedef struct task_s{
    int code;
    int netFd;
    struct task_s *pNext;
} task_t;

//任务队列，以及其互斥访问
typedef struct taskQueue_s{
    task_t *pFront;
    task_t *pRear;
    int size;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} taskQueue_t;

//线程池
typedef struct threadPool_s{
    pthread_t *tid;
    int threadNum;
    taskQueue_t taskQueue;
    int exitFlag;       //当前线程池是否结束运行
} threadPool_t;

// 线程池创建和传递命令所用函数
int taskEnqueue(taskQueue_t *pTaskQueue, int code, int netFd);
int taskDequeue(taskQueue_t *pTaskQueue);
int threadPoolInit(threadPool_t *pThreadPool, int workerNum);
int epollAdd(int fd, int epfd);
int epollDel(int fd, int epfd);
int tcpInit(int *pSockFd, char *file);
int makeWorker(threadPool_t *pThreadPool);

#endif
