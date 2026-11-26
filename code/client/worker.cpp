#include "threadPool.h"
#include <exception>
#include <utility>

// 启动指定数量的工作线程。创建失败时回收已启动的线程并返回 -1。
int ThreadPool::start(int workerNum)
{
    if (running || workerNum <= 0)
    {
        return -1;
    }
    int pipeFds[2];
    if (pipe(pipeFds) == -1)
    {
        return -1;
    }
    FileDescriptor readEnd(pipeFds[0]);
    FileDescriptor writeEnd(pipeFds[1]);
    if (socket_nonblock(readEnd.get()) != 0 || socket_nonblock(writeEnd.get()) != 0)
    {
        return -1;
    }
    // 两端都已设为非阻塞，把管道所有权交给线程池，等所有线程退出后再关闭。
    tokenPipe[0] = std::move(readEnd);
    tokenPipe[1] = std::move(writeEnd);
    pendingTasks = 0;
    tokenState = TokenState::closed;
    tokenVersion = 0;
    tokenDeadline = 0;
    token[0] = '\0';
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
        tokenState = TokenState::closed;
        while (!taskQueue.empty())
        {
            taskQueue.pop();
            // 成功、失败或超时都结束一个已领取任务的计数。
        --pendingTasks;
        }
    }
    // 队列锁已释放，唤醒后工作线程可以取锁并看到退出状态。
    taskQueue.cond.notify_all();
    tokenCond.notify_all();
    for (std::thread &worker : workers)
    {
        worker.join();
    }
    workers.clear();
    tokenPipe[0].reset();
    tokenPipe[1].reset();
}

// 循环取任务。队列状态在锁内处理，实际传输在锁外执行。
void ThreadPool::work()
{
    for (;;)
    {
        TransferTask task;
        {
            std::unique_lock<std::mutex> lock(taskQueue.mutex);
            // 等待时会释放互斥锁。被唤醒后持锁重查条件，防止空队列上误取任务。
            taskQueue.cond.wait(lock, [this] { return exitFlag || !taskQueue.empty(); });
            if (exitFlag)
            {
                return;
            }
            task = taskQueue.front();
            taskQueue.pop();
        }
        handle_long_cmd(&task, this);
        std::lock_guard<std::mutex> lock(taskQueue.mutex);
        // 成功、失败或超时都结束一个已领取任务的计数。
        --pendingTasks;
    }
}

int handle_long_cmd(TransferTask *pserver, ThreadPool *pThreadPool)
{
    size_t cmdLength = strnlen(pserver->cmd, sizeof(pserver->cmd));
    if (cmdLength < 6 || cmdLength >= sizeof(pserver->cmd) ||
       (strncmp(pserver->cmd, "puts ", 5) != 0 && strncmp(pserver->cmd, "gets ", 5) != 0))
    {
        return EXIT_FAILURE;
    }
    char file[100] = {0};
    char md5_str[MD5_STR_LEN + 1] = {0};
    bool uploading = strncmp(pserver->cmd, "puts ", 5) == 0;
    if (uploading)
    {
        int i = 4;
        while (pserver->cmd[i] == ' ')
        {
            ++i;
        }
        if (cmdLength - i == 0 || cmdLength - i >= sizeof(file))
        {
            return EXIT_FAILURE;
        }
        strcpy(file, pserver->cmd + i);
        // 计算MD5和上传使用同一文件及锁，避免中间重新打开路径
    }
    FileDescriptor localFile(uploading ? open_in_root("upload", file, O_RDONLY, 0) : -1);
    int fileFd = localFile.get();
    if (uploading)
    {
        if (fileFd == -1)
        {
            perror("open");
            return EXIT_FAILURE;
        }
        if (flock(fileFd, LOCK_SH | LOCK_NB) == -1 ||
           Compute_fd_md5(fileFd, md5_str) != 0)
        {
            return EXIT_FAILURE;
        }
    }
    char token[PROTOCOL_TOKEN_CAPACITY];
    unsigned long tokenVersion = 0;
    if (get_token(pThreadPool, token, &tokenVersion, 0) != 0)
    {
        printf("Cannot obtain token, please login again.\n");
        return EXIT_FAILURE;
    }
    FileDescriptor connection(socket(AF_INET, SOCK_STREAM, 0));
    int sockFd_child = connection.get();
    if (sockFd_child == -1)
    {
        perror("socket");
        return EXIT_FAILURE;
    }
    if (tcpInit(&sockFd_child, "./config/client.conf") != 0)
    {
        return EXIT_FAILURE;
    }
    if (socket_nonblock(sockFd_child) != 0)
    {
        return EXIT_FAILURE;
    }
    int check = 0;
    int times = 0;
    while (1)
    {
        // 发送token验证消息
        train_state_t train_state;
        int length = snprintf(train_state.buf, sizeof(train_state.buf), "%s%s", pserver->user_name, token);
        if (length < 0 || length >= (int)sizeof(train_state.buf))
        {
            return EXIT_FAILURE;
        }
        train_state.state = TOKEN;
        train_state.length = strlen(train_state.buf);
        train_state.pre_length = strlen(pserver->user_name);
        if (wire_send_command(sockFd_child, &train_state, 0) != (int)(sizeof(train_state.length) + sizeof(train_state.state) + sizeof(train_state.pre_length) + train_state.length))
        {
            return EXIT_FAILURE;
        }
        if (wire_recv_int(sockFd_child, &check) != sizeof(check))
        {
            return -1;
        }
        if (check == TOKEN_VALID)
        {
            // 验证成功退出
            break;
        }
        if (check != TOKEN_EXPIRED || times != 0 ||
           get_token(pThreadPool, token, &tokenVersion, 1) != 0)
        {
            printf("Token authentication failed, please login again.\n");
            return EXIT_FAILURE;
        }
        ++times;// 只有明确过期才续签，最多重试一次
    }

    // 验证成功发送命令
    train_t train;
    train.length = cmdLength;
    memcpy(train.buf, pserver->cmd, cmdLength);
    train.buf[cmdLength] = '\0';
    if (strncmp("puts ", train.buf, 5) != 0 && strncmp("gets ", train.buf, 5) != 0)
    {
        return EXIT_FAILURE;
    }
    if (wire_send_train(sockFd_child, &train, 0) != (int)(sizeof(train.length) + train.length))
    {
        return EXIT_FAILURE;
    }
    if (wire_send_int(sockFd_child, pserver->directoryId, MSG_NOSIGNAL) != sizeof(int))
    {
        return EXIT_FAILURE;
    }
    if (strncmp("puts ", train.buf, 5) == 0)
    {
        return Upload(sockFd_child, file, fileFd, md5_str);
    }
    else// 不是puts就是gets，其他的以后再说
    {
        return Download(sockFd_child);
    }
}
