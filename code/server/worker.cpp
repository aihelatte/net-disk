#include "threadPool.h"
#include <exception>
#include <functional>
#include <utility>

// 启动指定数量的工作线程。创建失败时回收已启动的线程并返回 -1。
int ThreadPool::start(int workerNum)
{
    if (running || workerNum <= 0)
    {
        return -1;
    }
    exitFlag = false;
    running = true;
    try
    {
        workers.reserve(workerNum);
        for (int i = 0; i < workerNum; ++i)
        {
            workers.emplace_back(&ThreadPool::work, this);
        }
    } catch(const std::exception &error) {
        fprintf(stderr, "Starting worker threads: %s\n", error.what());
        stop();
        return -1;
    }
    return 0;
}

// 停止领取新任务并清空队列。已领取的任务执行结束后再回收线程。
void ThreadPool::stop()
{
    if (!running)
    {
        return;
    }
    running = false;
    {
        std::lock_guard<std::mutex> lock(taskQueue.mutex);
        exitFlag = true;
        while (!taskQueue.empty())
        {
            taskQueue.pop();
        }
    }
    // 队列锁已释放，唤醒后工作线程可以取锁并看到退出状态。
    taskQueue.cond.notify_all();
    for (std::thread &worker : workers)
    {
        worker.join();
    }
    workers.clear();
}

// 循环取任务。队列状态在锁内处理，实际传输在锁外执行。
void ThreadPool::work()
{
    const size_t threadId = std::hash<std::thread::id>{}(std::this_thread::get_id());
    for (;;)
    {
        task_t task{};
        printf("I am free!tid = %zu\n", threadId);
        {
            std::unique_lock<std::mutex> lock(taskQueue.mutex);
            // 等待时会释放互斥锁。被唤醒后持锁重查条件，防止空队列上误取任务。
            taskQueue.cond.wait(lock, [this] { return exitFlag || !taskQueue.empty(); });
            if (exitFlag)
            {
                return;
            }
            // 队列把数据连接交给当前任务，弹出旧条目时不会关闭这个 fd。
            task = std::move(taskQueue.front());
            taskQueue.pop();
        }
        printf("I am working! tid = %zu\n", threadId);
        handle_long_cmd(task.netFd.get(), task.code);
        puts("done!");
    }
}

int handle_long_cmd(int netFd, int user_id)
{
    puts("I am long cmd!");
    // 接收命令
    char cmd[100] = {0};
    int datalength;
    if (wire_recv_int(netFd, &datalength) != sizeof(datalength) ||
       datalength < 6 || datalength >= (int)sizeof(cmd) ||
       recvn(netFd, cmd, datalength) != datalength)
    {
        return EXIT_FAILURE;
    }
    if (memchr(cmd, '\0', datalength) != NULL)
    {
        return EXIT_FAILURE;
    }
    int directoryId;
    if (wire_recv_int(netFd, &directoryId) != sizeof(int) || directoryId < 0)
    {
        return EXIT_FAILURE;
    }
    sysadd(user_id, cmd);


    if (strncmp("puts ", cmd, 5) == 0)
    {
        return Download(netFd, user_id, directoryId);
    }
    else if (strncmp("gets ", cmd, 5) == 0)//gets
    {
        char file[100] = {0};
        int i = 4;
        while (cmd[i] == ' ')
        {
            ++i;
        }
        if (datalength - i <= 0 || datalength - i >= (int)sizeof(file))
        {
            return EXIT_FAILURE;
        }
        strcpy(file, cmd + i);
        return Upload(netFd, file, user_id, directoryId);
    }
    else
    {
        return EXIT_FAILURE;
    }
    return 0;
}
