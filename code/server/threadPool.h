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

//一个任务单元结构体
struct task_t
{
    int code;
    FileDescriptor netFd;
    int64_t queuedAt;
};

// 持有待执行任务和同步对象。调用方用同一把锁检查退出状态和操作队列。
class TaskQueue
{
public:
    bool empty() const { return tasks.empty(); }
    task_t &front() { return tasks.front(); }
    void pop() { tasks.pop(); }
    int push(int code, FileDescriptor &netFd);
    void expire(int64_t now);
    std::mutex mutex;
    std::condition_variable cond;
private:
    std::queue<task_t> tasks;
};

//线程池
// 管理传输工作线程，退出时清理待执行任务并等待正在处理的线程。
class ThreadPool
{
public:
    ThreadPool() = default;
    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;
    int start(int workerNum);
    void stop();
    ~ThreadPool() { stop(); }
    TaskQueue taskQueue;
private:
    void work();
    bool exitFlag = false;
    bool running = false;
    std::vector<std::thread> workers;
};

int epollAdd(int fd, int epfd);
int epollDel(int fd, int epfd);
int epollMod(int fd, int epfd, uint32_t events);
int tcpInit(int *pSockFd, const char *file);

#endif
