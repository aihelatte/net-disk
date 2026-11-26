#ifndef _THREADPOOL_H// 头文件在编译时被多次引用，避免重复定义
#define _THREADPOOL_H
#include<myhead.h>
#include "file_descriptor.h"
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include "worker.h"

struct TransferTask
{
    int directoryId;
    char cmd[100];
    char user_name[100];
};

// 持有待执行任务和同步对象。调用方用同一把锁检查退出状态和操作队列。
class TaskQueue
{
public:
    bool empty() const { return tasks.empty(); }
    const TransferTask &front() const { return tasks.front(); }
    void pop() { tasks.pop(); }
    int push(const TransferTask &task);
    std::mutex mutex;
    std::condition_variable cond;
private:
    std::queue<TransferTask> tasks;
};

enum class TokenState
{
    ready,
    requested,
    refreshing,
    closed,
};

// 管理传输工作线程和 Token 通知管道，析构时等待线程退出。
class ThreadPool
{
public:
    ThreadPool() = default;
    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;
    int start(int workerNum);
    void stop();
    void setPrompt(const char *userName, const char *pwd);
    void reprintPrompt();
    ~ThreadPool() { stop(); }
    TaskQueue taskQueue;
    char token[PROTOCOL_TOKEN_CAPACITY];
    int64_t tokenDeadline;
    unsigned long tokenVersion;
    TokenState tokenState = TokenState::closed;// 凭证状态也使用队列锁保护，不另加互斥锁
    std::condition_variable tokenCond;
    FileDescriptor tokenPipe[2];// 工作线程只通知主线程，不直接读写控制连接
    int pendingTasks;// 排队中和执行中的传输任务总数，使用队列锁保护
private:
    void work();
    bool exitFlag = false;
    bool running = false;
    // 用户名为空表示尚未登录，两个字段都由 taskQueue.mutex 保护。
    char promptUserName[PROTOCOL_USER_NAME_CAPACITY] = {};
    char promptDirectory[1024] = {};
    std::vector<std::thread> workers;
};

int epollAdd(int fd, int epfd);
int tcpInit(int *pSockFd, const char *file);

int handle_long_cmd(TransferTask *pserver, ThreadPool *pThreadPool);
int receive_token(int sockFd, char *token, int64_t *pDeadline);
int get_token(ThreadPool *pThreadPool, char *token,
              unsigned long *pVersion, int forceRefresh);
int refresh_token(int sockFd, ThreadPool *pThreadPool);
void close_token(ThreadPool *pThreadPool);

#endif
