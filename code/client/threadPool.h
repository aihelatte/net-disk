#ifndef _THREADPOOL_H// 头文件在编译时被多次引用，避免重复定义
#define _THREADPOOL_H
#include<myhead.h>
#include "worker.h"

typedef struct server_s{// 记录当前用户的信息
    char cmd[100];
    char token[1024];
    char key[100];
    char user_name[100];
} server_t;

//一个任务单元
typedef struct task_s{
    server_t *pserver;
    int netFd;
    struct task_s *pNext;
} task_t;

//一个任务队列
typedef struct taskQueue_s{
    task_t *pFront;
    task_t *pRear;
    int size;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} taskQueue_t;

//包含了任务队列的线程池结构体
typedef struct threadPool_s{
    pthread_t *tid;
    int threadNum;
    taskQueue_t taskQueue;
    int exitFlag;
} threadPool_t;

int taskEnqueue(taskQueue_t *pTaskQueue, server_t *pserver);
int taskDequeue(taskQueue_t *pTaskQueue);
int threadPoolInit(threadPool_t *pThreadPool, int workerNum);
int epollAdd(int fd, int epfd);
int epollDel(int fd, int epfd);
int tcpInit(int *pSockFd, char *file);
int makeWorker(threadPool_t *pThreadPool);
int transFile(int netFd);
int handle_long_cmd(server_t *pserver);

#endif
