#ifndef _THREADPOOL_H// 头文件在编译时被多次引用，避免重复定义
#define _THREADPOOL_H
#include<myhead.h>
#include <queue>
#include <vector>
#include "worker.h"

struct TransferTask {
    int directoryId;
    char cmd[100];
    char user_name[100];
};

// 调用方在同一把锁下检查退出状态、等待条件和操作队列。
class TaskQueue {
public:
    bool empty() const { return tasks.empty(); }
    const TransferTask &front() const { return tasks.front(); }
    void pop() { tasks.pop(); }
    int push(const TransferTask &task);
    pthread_mutex_t mutex;
    pthread_cond_t cond;
private:
    std::queue<TransferTask> tasks;
};

//包含了任务队列的线程池结构体
enum token_state{
    TOKEN_READY,
    TOKEN_REQUESTED,
    TOKEN_REFRESHING,
    TOKEN_CLOSED,
};

class ThreadPool {
public:
    ThreadPool() = default;
    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;
    int start(int workerNum);
    void stop();
    ~ThreadPool() { stop(); }
    TaskQueue taskQueue;
    int exitFlag;
    char token[PROTOCOL_TOKEN_CAPACITY];
    int64_t tokenDeadline;
    unsigned long tokenVersion;
    int tokenState;// 凭证状态也使用队列锁保护，不另加互斥锁
    pthread_cond_t tokenCond;
    int tokenPipe[2];// 工作线程只通知主线程，不直接读写控制连接
    int pendingTasks;// 排队中和执行中的传输任务总数，使用队列锁保护
private:
    int initialize(int workerNum);
    int createWorkers();
    bool running = false;
    std::vector<pthread_t> tid;
    int threadNum = 0;
};

int epollAdd(int fd, int epfd);
int epollDel(int fd, int epfd);
int tcpInit(int *pSockFd, const char *file);

int transFile(int netFd);
int handle_long_cmd(TransferTask *pserver, ThreadPool *pThreadPool);
int receive_token(int sockFd, char *token, int64_t *pDeadline);
int get_token(ThreadPool *pThreadPool, char *token,
              unsigned long *pVersion, int forceRefresh);
int refresh_token(int sockFd, ThreadPool *pThreadPool);
void close_token(ThreadPool *pThreadPool);

#endif
