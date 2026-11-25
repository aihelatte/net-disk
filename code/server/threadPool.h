#ifndef _THREADPOOL_H// 头文件在编译时被多次引用，避免重复定义
#define _THREADPOOL_H
#include<myhead.h>
#include <queue>
#include <vector>
#include "worker.h"

//一个任务单元结构体
struct task_t {
    int code;
    int netFd;
    int64_t queuedAt;
};

// 调用方在同一把锁下检查退出状态、等待条件和操作队列。
class TaskQueue {
public:
    bool empty() const { return tasks.empty(); }
    const task_t &front() const { return tasks.front(); }
    void pop() { tasks.pop(); }
    int push(int code, int netFd);
    void expire(int64_t now);
    pthread_mutex_t mutex;
    pthread_cond_t cond;
private:
    std::queue<task_t> tasks;
};

//线程池
class ThreadPool {
public:
    ThreadPool() = default;
    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;
    int start(int workerNum);
    void stop();
    ~ThreadPool() { stop(); }
    TaskQueue taskQueue;
    int exitFlag;       //当前线程池是否结束运行
private:
    int initialize(int workerNum);
    int createWorkers();
    bool running = false;
    std::vector<pthread_t> tid;
    int threadNum = 0;
};

int epollAdd(int fd, int epfd);
int epollDel(int fd, int epfd);
int epollMod(int fd, int epfd, uint32_t events);
int tcpInit(int *pSockFd, const char *file);

#endif
