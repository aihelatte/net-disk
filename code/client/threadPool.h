#ifndef _THREADPOOL_H// 头文件在编译时被多次引用，避免重复定义
#define _THREADPOOL_H
#include<myhead.h>
#include "worker.h"

typedef struct server_s{// 记录当前用户的信息
    char cmd[100];
    char user_name[100];
} server_t;

//一个任务单元
typedef struct task_s{
    server_t server;
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
enum token_state{
    TOKEN_READY,
    TOKEN_REQUESTED,
    TOKEN_REFRESHING,
    TOKEN_CLOSED,
};

typedef struct threadPool_s{
    pthread_t *tid;
    int threadNum;
    taskQueue_t taskQueue;
    int exitFlag;
    char token[PROTOCOL_TOKEN_CAPACITY];
    int64_t tokenDeadline;
    unsigned long tokenVersion;
    int tokenState;// 凭证状态也使用队列锁保护，不另加互斥锁
    pthread_cond_t tokenCond;
    int tokenPipe[2];// 工作线程只通知主线程，不直接读写控制连接
    int pendingTasks;// 排队中和执行中的传输任务总数，使用队列锁保护
} threadPool_t;

int taskEnqueue(taskQueue_t *pTaskQueue, server_t *pserver);
int taskDequeue(taskQueue_t *pTaskQueue);
int threadPoolInit(threadPool_t *pThreadPool, int workerNum);
int epollAdd(int fd, int epfd);
int epollDel(int fd, int epfd);
int tcpInit(int *pSockFd, char *file);
int makeWorker(threadPool_t *pThreadPool);
int transFile(int netFd);
int handle_long_cmd(server_t *pserver, threadPool_t *pThreadPool);
int receive_token(int sockFd, char *token, int64_t *pDeadline);
int get_token(threadPool_t *pThreadPool, char *token,
              unsigned long *pVersion, int forceRefresh);
int refresh_token(int sockFd, threadPool_t *pThreadPool);
void close_token(threadPool_t *pThreadPool);

#endif
