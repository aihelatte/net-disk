#include "threadPool.h"
#include "worker.h"
constexpr int WORKER_COUNT = 3;

static volatile sig_atomic_t exitWriteFd = -1;

static void close_client_connection(int netFd, int epfd, ClientManager *clients)
{
    epollDel(netFd, epfd);
    clients->remove(netFd);
}

static void close_invalid_connection(int netFd, int epfd, ClientManager *clients, const char *reason)
{
    fprintf(stderr, "Invalid protocol message: fd=%d, reason=%s\n", netFd, reason);
    close_client_connection(netFd, epfd, clients);
}
// 根据当前会话阶段处理认证或已校验的命令。返回 -1 时主循环关闭连接。
static int dispatch_request(Connection *pConnection, ClientManager *clients)
{
    if (pConnection->stage == RECEIVE_REGISTER_PASSWORD)
    {
        return register_user(pConnection);
    }
    if (pConnection->stage == RECEIVE_LOGIN_PASSWORD)
    {
        return login(pConnection);
    }
    train_state_t train_get = pConnection->train;
    int user_id = pConnection->userId;
    if ((((train_get.state >= LS && train_get.state <= PUTS) ||
         train_get.state == KEEPALIVE || train_get.state == REFRESH) && user_id < 0) ||
       ((train_get.state == REGISTER || train_get.state == LOGIN || train_get.state == TOKEN) && user_id >= 0))
    {
        return -1;
    }
    switch (train_get.state)
    {
    case REGISTER:
        return register_user(pConnection);
    case LOGIN:
        return login(pConnection);
    case LS:
    case CD:
    case PWD:
    case REMOVE:
    case MKDIR:
        return handle_short_cmd(train_get, pConnection, user_id);
    case PUTS:
    case GETS:
        if (wire_queue_int(pConnection, pConnection->directoryId) != sizeof(int))
        {
            return -1;
        }
        return 0;
    case KEEPALIVE:
        return 0;
    case REFRESH:
        return refresh_token(pConnection);
    case TOKEN:
    {
        int user_id = token(pConnection, clients);
        if (user_id == -2)
        {
            return -1;
        }
        pConnection->transfer_user_id = user_id;
        return 0;
    }
    default:
        return -1;
    }
}

// 将认证后的数据连接从 epoll 交给线程池。入队成功返回 0，失败由主循环关闭连接。
static int handoff_connection(int netFd, int epfd, ClientManager *clients,
                              ThreadPool *pThreadPool)
{
    Connection *pConnection = clients->find(netFd);
    if (epollDel(netFd, epfd) != 0)
    {
        return -1;
    }
    std::lock_guard<std::mutex> lock(pThreadPool->taskQueue.mutex);
    // 只有任务入队成功才移交套接字，失败时 Connection 仍负责关闭它。
    int ret = pThreadPool->taskQueue.push(pConnection->transfer_user_id, pConnection->socketForHandoff());
    if (ret == 0)
    {
        clients->remove(netFd);
        pThreadPool->taskQueue.cond.notify_one();
    }
    return ret;
}

static void sigFunc(int)
{
    int savedErrno = errno;
    ssize_t ret;
    do
    {
        ret = write(exitWriteFd, "1", 1);
    } while (ret == -1 && errno == EINTR);
    errno = savedErrno;
}
// 初始化监听、线程池和时间轮。主线程处理连接事件，收到退出信号后停止接入并等待工作线程。
int main()
{
    signal(SIGPIPE, SIG_IGN);
    if (init_token_key() != 0)
    {
        fprintf(stderr, "Failed to load token signing key.\n");
        return EXIT_FAILURE;
    }
    //管理一个客户端断开的管道
    int exitPipe[2];
    if (pipe(exitPipe) == -1)
    {
        return EXIT_FAILURE;
    }
    FileDescriptor exitRead(exitPipe[0]);
    FileDescriptor exitWrite(exitPipe[1]);
    exitWriteFd = exitWrite.get();
    // 制作子线程
    ThreadPool threadPool;
    if (threadPool.start(WORKER_COUNT) != 0)
    {
        fprintf(stderr, "Failed to initialize thread pool.\n");
        return EXIT_FAILURE;
    }

    // 绑定ip和端口号
    int sockFd = -1;
    if (tcpInit(&sockFd, "./config/server.conf") != 0)
    {
        return EXIT_FAILURE;
    } // 以读取config文件的形式建立连接

    FileDescriptor listener(sockFd);
    //定义要监听的集合
    FileDescriptor epollFd(epoll_create(1));
    int epfd = epollFd.get();

    // 创建客户队列并初始化
    ClientManager clients;
    if (clients.initialize() != 0)
    {
        fprintf(stderr, "Failed to initialize client queue.\n");
        return EXIT_FAILURE;
    }

    // 初始化本地文件表（用于秒传），确保数据库信息和本地信息一致
    if (init_local_file() != 0)
    {
        fprintf(stderr, "Failed to initialize local files.\n");
        return EXIT_FAILURE;
    }

    // 开始监听
    if (epfd == -1 || socket_nonblock(sockFd) != 0 || socket_nonblock(exitWrite.get()) != 0 ||
       epollAdd(sockFd, epfd) != 0 || epollAdd(exitRead.get(), epfd) != 0)
    {
        return EXIT_FAILURE;
    }
    struct sigaction action;
    bzero(&action, sizeof(action));
    action.sa_handler = sigFunc;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1 ||
       sigaction(SIGTERM, &action, NULL) == -1 ||
       sigaction(SIGUSR1, &action, NULL) == -1)
    {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    struct epoll_event readyArr[3];
    int ret;
    struct timespec tick;
    if (clock_gettime(CLOCK_MONOTONIC, &tick) == -1)
    {
        return EXIT_FAILURE;
    }
    int64_t lastTick = (int64_t)tick.tv_sec * 1000 + tick.tv_nsec / 1000000;
    while (1)
    {
        if (clock_gettime(CLOCK_MONOTONIC, &tick) == -1)
        {
            return EXIT_FAILURE;
        }
        int64_t waitNow = (int64_t)tick.tv_sec * 1000 + tick.tv_nsec / 1000000;
        int64_t remaining = lastTick + 100 - waitNow;
        int waitTime = remaining > 0 ? (int)remaining : 0;
        int readyNum = epoll_wait(epfd, readyArr, 3, waitTime);// 等到下一格到期，已到期则立即返回

        if (readyNum == -1 && errno != EINTR)
        {
            perror("epoll_wait");
            return EXIT_FAILURE;
        }
        if (readyNum == -1)
        {
            readyNum = 0;
        }
        int exiting = 0;
        for (int i = 0; i < readyNum; ++i)
        {
            if (readyArr[i].data.fd == exitRead.get())
            {
                exiting = 1;
                break;
            }
        }
        if (exiting)
        {
            break;
        }
        if (clock_gettime(CLOCK_MONOTONIC, &tick) == -1)
        {
            return EXIT_FAILURE;
        }
        int64_t now = (int64_t)tick.tv_sec * 1000 + tick.tv_nsec / 1000000;
        // 先清理过期任务和连接，再分发这批 epoll 事件，避免处理刚关闭的 fd。
        while (now - lastTick >= 100)// 每经过100ms推进一格，与是否有事件无关
        {
            {
                std::lock_guard<std::mutex> lock(threadPool.taskQueue.mutex);
                threadPool.taskQueue.expire(now);
            }
            for (int netFd = clients.nextExpired(); netFd >= 0; netFd = clients.nextExpired())
            {
                Connection *pConnection = clients.find(netFd);
                if (pConnection->userId >= 0 && !pConnection->hasReply() &&
                   !pConnection->hasPartialRequest())// 只向空闲控制连接发送超时通知
                {
                    unsigned char check[4];
                    wire_put_i32(check, 1);
                    // 通知尽力发送，清理不依赖客户端回应，也不等待发送缓冲区
                    send(netFd, check, sizeof(check), MSG_NOSIGNAL | MSG_DONTWAIT);
                }
                close_client_connection(netFd, epfd, &clients);
                for (int i = 0; i < readyNum; ++i)
                {
                    if (readyArr[i].data.fd == netFd)
                    {
                        readyArr[i].data.fd = -1;
                    }
                }
            }
            clients.advanceTimer();
            lastTick += 100;
        }

        for (int i = 0; i < readyNum; ++i)
        {
            if (readyArr[i].data.fd == -1)
            {
                continue;
            }
            if (readyArr[i].data.fd == sockFd)// 监听到建立连接请求
            {
                // 监听socket也为非阻塞，每次事件最多接受32个连接
                for (int accepted = 0; accepted < 32; ++accepted)
                {
                    int netFd = accept(sockFd, NULL, NULL);
                    if (netFd == -1)
                    {
                        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                        {
                            perror("accept");
                        }
                        break;
                    }
                    FileDescriptor acceptedSocket(netFd);
                    if (socket_nonblock(netFd) != 0 || clients.add(std::move(acceptedSocket)) != 0)
                    {
                        continue;
                    }
                    if (epollAdd(netFd, epfd) != 0)
                    {
                        clients.remove(netFd);
                    }
                }
            }
            else// 监听到客户端的消息或回复可以继续发送
            {
                int netFd = readyArr[i].data.fd;
                Connection *pConnection = clients.find(netFd);
                if (pConnection == NULL)
                {
                    continue;
                }
                if (readyArr[i].events & (EPOLLERR | EPOLLHUP))
                {
                    close_client_connection(netFd, epfd, &clients);
                    continue;
                }
                if (!pConnection->hasReply() && (readyArr[i].events & EPOLLIN))
                {
                    bool progress = false;
                    ret = pConnection->receiveRequest(progress);
                    if (progress)
                    {
                        clients.touch(netFd);
                    }
                    if (ret < 0)
                    {
                        close_invalid_connection(netFd, epfd, &clients, "incomplete or invalid message");
                        continue;
                    }
                    if (ret == 1)
                    {
                        if (dispatch_request(pConnection, &clients) != 0)
                        {
                            close_invalid_connection(netFd, epfd, &clients, "command failed or is not allowed");
                            continue;
                        }
                        pConnection->resetRequest();
                    }
                }
                if (pConnection->hasReply())
                {
                    ret = pConnection->flushReply();
                    if (ret > 0)
                    {
                        clients.touch(netFd);
                    }
                    if (ret < 0)
                    {
                        close_client_connection(netFd, epfd, &clients);
                        continue;
                    }
                }
                // Token回复全部发出后才交接，工作线程不会与主线程同时收发
                if (!pConnection->hasReply() && pConnection->transfer_user_id >= 0)
                {
                    if (handoff_connection(netFd, epfd, &clients, &threadPool) != 0)
                    {
                        close_client_connection(netFd, epfd, &clients);
                    }
                    continue;
                }
                // 回复还有剩余时只监听可写事件，发完再读下一条请求。
                uint32_t events = !pConnection->hasReply() ? EPOLLIN : EPOLLOUT;
                if (epollMod(netFd, epfd, events) != 0)
                {
                    close_client_connection(netFd, epfd, &clients);
                }
            }
        }
    }

    listener.reset();
    threadPool.stop();
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGUSR1, SIG_IGN);
    exitWriteFd = -1;
    return 0;
}
