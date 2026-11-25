#include "threadPool.h"
#include "worker.h"
#include "protocol_validate.h"
#define NUM 3       //线程池中子线程数量，固定为3

int exitPipe[2];

static int is_valid_train_header(const train_state_t *pTrain)
{
    if(!protocol_valid_text_length(pTrain->length, sizeof(pTrain->buf)) ||
       pTrain->state < REGISTER || pTrain->state > REFRESH ||
       !protocol_valid_pre_length(pTrain->length, pTrain->pre_length))
    {
        return 0;
    }

    switch(pTrain->state)
    {
    case REGISTER:
    case LOGIN:
        return protocol_valid_text_length(pTrain->length,
                                          PROTOCOL_USER_NAME_CAPACITY) &&
               pTrain->length > 0 && pTrain->pre_length == 0;
    case LS:
        return protocol_valid_text_length(pTrain->length,
                                          PROTOCOL_COMMAND_CAPACITY) &&
               pTrain->length >= 2 && pTrain->pre_length == 2;
    case CD:
    case REMOVE:
        return protocol_valid_text_length(pTrain->length,
                                          PROTOCOL_COMMAND_CAPACITY) &&
               pTrain->length > 3 && pTrain->pre_length == 2;
    case PWD:
        return protocol_valid_text_length(pTrain->length,
                                          PROTOCOL_COMMAND_CAPACITY) &&
               pTrain->length == 3 && pTrain->pre_length == 3;
    case MKDIR:
        return protocol_valid_text_length(pTrain->length,
                                          PROTOCOL_COMMAND_CAPACITY) &&
               pTrain->length > 6 && pTrain->pre_length == 5;
    case GETS:
    case PUTS:
        return protocol_valid_text_length(pTrain->length,
                                          PROTOCOL_COMMAND_CAPACITY) &&
               pTrain->length > 5 && pTrain->pre_length == 4;
    case TOKEN:
        return protocol_valid_split_text(pTrain->length,
                                         pTrain->pre_length,
                                         PROTOCOL_USER_NAME_CAPACITY,
                                         PROTOCOL_TOKEN_CAPACITY);
    case KEEPALIVE:
        return pTrain->length == 4 && pTrain->pre_length == 4;
    case REFRESH:
        return pTrain->length == 7 && pTrain->pre_length == 7;
    default:
        return 0;
    }
}

static int is_valid_train_body(const train_state_t *pTrain)
{
    switch(pTrain->state)
    {
    case KEEPALIVE:
        return strcmp(pTrain->buf, "ping") == 0;
    case REFRESH:
        return strcmp(pTrain->buf, "refresh") == 0;
    case REGISTER:
    case LOGIN:
    case TOKEN:
        return 1;
    case LS:
        return strncmp(pTrain->buf, "ls", 2) == 0 &&
               (pTrain->length == 2 || pTrain->buf[2] == ' ');
    case CD:
        return strncmp(pTrain->buf, "cd ", 3) == 0;
    case PWD:
        return strcmp(pTrain->buf, "pwd") == 0;
    case REMOVE:
        return strncmp(pTrain->buf, "rm ", 3) == 0;
    case MKDIR:
        return strncmp(pTrain->buf, "mkdir ", 6) == 0;
    case GETS:
        return strncmp(pTrain->buf, "gets ", 5) == 0;
    case PUTS:
        return strncmp(pTrain->buf, "puts ", 5) == 0;
    default:
        return 0;
    }
}

static void release_connection(int netFd, clientQueue_t *pclientQueue)
{
    pclientQueue->connections[netFd].reset();
}

static void close_client_connection(int netFd, int epfd, clientQueue_t *pclientQueue)
{
    fdDel(netFd, pclientQueue);
    pclientQueue->client[netFd] = -1;
    release_connection(netFd, pclientQueue);
    epollDel(netFd, epfd);
    close(netFd);
}

static void close_invalid_connection(int netFd, int epfd, clientQueue_t *pclientQueue, const char *reason)
{
    fprintf(stderr, "Invalid protocol message: fd=%d, reason=%s\n", netFd, reason);
    close_client_connection(netFd, epfd, pclientQueue);
}
static void reset_request(connection_t *pConnection)
{
    bzero(&pConnection->train, sizeof(pConnection->train));
    pConnection->received = 0;
    pConnection->needed = sizeof(int);
}

// 一次只收当前帧需要的字节，避免吞掉随后交给工作线程的文件命令
static int receive_request(connection_t *pConnection, int *progress)
{
    while(1)
    {
        ssize_t ret = recv(pConnection->netFd,
                           (char *)&pConnection->train + pConnection->received,
                           pConnection->needed - pConnection->received,
                           MSG_DONTWAIT);
        if(ret > 0)
        {
            *progress = 1;
            pConnection->received += ret;
            if(pConnection->received < pConnection->needed)
            {
                continue;
            }
            if(pConnection->received == sizeof(int))
            {
                int length = wire_get_i32(&pConnection->train.length);
                pConnection->train.length = length;
                if(pConnection->stage == RECEIVE_COMMAND)
                {
                    if(length <= 0 || !protocol_valid_text_length(length, sizeof(pConnection->train.buf)))
                    {
                        return -1;
                    }
                    pConnection->needed = 3 * sizeof(int);
                }
                else
                {
                    if(length != 49)
                    {
                        return -1;
                    }
                    pConnection->needed = sizeof(int) + length;
                }
                continue;
            }
            if(pConnection->stage == RECEIVE_COMMAND &&
               pConnection->received == 3 * sizeof(int))
            {
                pConnection->train.state = wire_get_i32(&pConnection->train.state);
                pConnection->train.pre_length = wire_get_i32(&pConnection->train.pre_length);
                if(!is_valid_train_header(&pConnection->train))
                {
                    return -1;
                }
                pConnection->needed = 3 * sizeof(int) + pConnection->train.length;
                continue;
            }
            if(pConnection->stage == RECEIVE_COMMAND)
            {
                pConnection->train.buf[pConnection->train.length] = '\0';
                if(memchr(pConnection->train.buf, '\0', pConnection->train.length) != NULL ||
                   !is_valid_train_body(&pConnection->train))
                {
                    return -1;
                }
            }
            else if(memchr((char *)&pConnection->train + sizeof(int), '\0', 49) != NULL)
            {
                return -1;
            }
            return 1;
        }
        if(ret == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        {
            return 0;// 半包保留在连接中，回到epoll处理其他连接
        }
        return -1;
    }
}

static int dispatch_request(connection_t *pConnection, clientQueue_t *pclientQueue)
{
    if(pConnection->stage == RECEIVE_REGISTER_PASSWORD)
    {
        return register_user(pConnection);
    }
    if(pConnection->stage == RECEIVE_LOGIN_PASSWORD)
    {
        return login(pConnection, pclientQueue);
    }
    train_state_t train_get = pConnection->train;
    int user_id = pclientQueue->client[pConnection->netFd];
    if((((train_get.state >= LS && train_get.state <= PUTS) ||
         train_get.state == KEEPALIVE || train_get.state == REFRESH) && user_id < 0) ||
       ((train_get.state == REGISTER || train_get.state == LOGIN || train_get.state == TOKEN) && user_id >= 0))
    {
        return -1;
    }
    switch(train_get.state)
    {
    case REGISTER:
        return register_user(pConnection);
    case LOGIN:
        return login(pConnection, pclientQueue);
    case LS:
    case CD:
    case PWD:
    case REMOVE:
    case MKDIR:
        return handle_short_cmd(train_get, pConnection, user_id);
    case PUTS:
    case GETS:
        return wire_queue_int(pConnection, pConnection->directoryId) == sizeof(int) ? 0 : -1;
    case KEEPALIVE:
        return 0;
    case REFRESH:
        return refresh_token(pConnection);
    case TOKEN:
    {
        int user_id = token(pConnection, pclientQueue);
        if(user_id == -2)
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

static int handoff_connection(int netFd, int epfd, clientQueue_t *pclientQueue,
                              ThreadPool *pThreadPool)
{
    connection_t *pConnection = pclientQueue->connections[netFd].get();
    if(epollDel(netFd, epfd) != 0)
    {
        return -1;
    }
    fdDel(netFd, pclientQueue);
    pthread_mutex_lock(&pThreadPool->taskQueue.mutex);
    int ret = pThreadPool->taskQueue.push(pConnection->transfer_user_id, netFd);
    if(ret == 0)
    {
        release_connection(netFd, pclientQueue);
        pthread_cond_signal(&pThreadPool->taskQueue.cond);
    }
    pthread_mutex_unlock(&pThreadPool->taskQueue.mutex);
    return ret;
}

void sigFunc(int signum)
{
    int savedErrno = errno;
    ssize_t ret;
    do
    {
        ret = write(exitPipe[1], "1", 1);
    } while(ret == -1 && errno == EINTR);
    errno = savedErrno;
}
int ThreadPool::start(int workerNum)
{
    if(running || workerNum <= 0 || initialize(workerNum) != 0)
        return -1;
    return createWorkers();
}

void ThreadPool::stop()
{
    if(!running) return;
    running = false;
    pthread_mutex_lock(&taskQueue.mutex);
    exitFlag = 1;
    while(!taskQueue.empty())
    {
        int netFd = taskQueue.front().netFd;
        taskQueue.pop();
        close(netFd);
    }
    pthread_cond_broadcast(&taskQueue.cond);
    pthread_mutex_unlock(&taskQueue.mutex);
    for(int i = 0; i < threadNum; ++i)
    {
        pthread_join(tid[i], NULL);
    }
    pthread_cond_destroy(&taskQueue.cond);
    pthread_mutex_destroy(&taskQueue.mutex);
}

int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);
    if(init_token_key() != 0)
    {
        fprintf(stderr, "Failed to load token signing key.\n");
        return EXIT_FAILURE;
    }
    //管理一个客户端断开的管道
    if(pipe(exitPipe) == -1)
    {
        return EXIT_FAILURE;
    }
    // 制作子线程
    int workerNum = NUM;
    ThreadPool threadPool;
    if(threadPool.start(workerNum) != 0)
    {
        fprintf(stderr, "Failed to initialize thread pool.\n");
        return EXIT_FAILURE;
    }
    
    // 绑定ip和端口号
    int sockFd;
    if(tcpInit(&sockFd, "./config/server.conf") != 0)
    {
        return EXIT_FAILURE;
    } // 以读取config文件的形式建立连接

    //定义要监听的集合
    int epfd = epoll_create(1);

    // 创建客户队列并初始化
    clientQueue_t clientQueue;
    if(init_clientQueue(&clientQueue) != 0)
    {
        fprintf(stderr, "Failed to initialize client queue.\n");
        return EXIT_FAILURE;
    }

    // 初始化本地文件表（用于秒传），确保数据库信息和本地信息一致
    if(init_local_file() != 0)
    {
        fprintf(stderr, "Failed to initialize local files.\n");
        return EXIT_FAILURE;
    }

    // 开始监听
    if(epfd == -1 || socket_nonblock(sockFd) != 0 || socket_nonblock(exitPipe[1]) != 0 ||
       epollAdd(sockFd, epfd) != 0 || epollAdd(exitPipe[0], epfd) != 0)
    {
        close(sockFd);
        return EXIT_FAILURE;
    }
    struct sigaction action;
    bzero(&action, sizeof(action));
    action.sa_handler = sigFunc;
    sigemptyset(&action.sa_mask);
    if(sigaction(SIGINT, &action, NULL) == -1 ||
       sigaction(SIGTERM, &action, NULL) == -1 ||
       sigaction(SIGUSR1, &action, NULL) == -1)
    {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    struct epoll_event readyArr[3];
    int ret;
    struct timespec tick;
    if(clock_gettime(CLOCK_MONOTONIC, &tick) == -1)
    {
        return EXIT_FAILURE;
    }
    int64_t lastTick = (int64_t)tick.tv_sec * 1000 + tick.tv_nsec / 1000000;
    while (1)
    {
        if(clock_gettime(CLOCK_MONOTONIC, &tick) == -1)
        {
            return EXIT_FAILURE;
        }
        int64_t waitNow = (int64_t)tick.tv_sec * 1000 + tick.tv_nsec / 1000000;
        int64_t remaining = lastTick + 100 - waitNow;
        int waitTime = remaining > 0 ? (int)remaining : 0;
        // printf("epoll_wait!\n");
        int readyNum = epoll_wait(epfd, readyArr, 3, waitTime);// 等到下一格到期，已到期则立即返回
        // printf("epoll_wait ready!\n");

        if(readyNum == -1 && errno != EINTR)
        {
            perror("epoll_wait");
            return EXIT_FAILURE;
        }
        if(readyNum == -1)
        {
            readyNum = 0;
        }
        int exiting = 0;
        for(int i = 0; i < readyNum; ++i)
        {
            if(readyArr[i].data.fd == exitPipe[0])
            {
                exiting = 1;
                break;
            }
        }
        if(exiting)
        {
            break;
        }
        if(clock_gettime(CLOCK_MONOTONIC, &tick) == -1)
        {
            return EXIT_FAILURE;
        }
        int64_t now = (int64_t)tick.tv_sec * 1000 + tick.tv_nsec / 1000000;
        while(now - lastTick >= 100)// 每经过100ms推进一格，与是否有事件无关
        {
            pthread_mutex_lock(&threadPool.taskQueue.mutex);
            threadPool.taskQueue.expire(now);
            pthread_mutex_unlock(&threadPool.taskQueue.mutex);
            while(!clientQueue.time_out[clientQueue.timer].empty())
            {
                int netFd = clientQueue.time_out[clientQueue.timer].front();
                connection_t *pConnection = clientQueue.connections[netFd].get();
                if(clientQueue.client[netFd] >= 0 && !pConnection->hasReply() &&
                   pConnection->received == 0)// 只向空闲控制连接发送超时通知
                {
                    unsigned char check[4];
                    wire_put_i32(check, 1);
                    // 通知尽力发送，清理不依赖客户端回应，也不等待发送缓冲区
                    send(netFd, check, sizeof(check), MSG_NOSIGNAL | MSG_DONTWAIT);
                }
                close_client_connection(netFd, epfd, &clientQueue);
                for(int i = 0; i < readyNum; ++i)
                {
                    if(readyArr[i].data.fd == netFd)
                    {
                        readyArr[i].data.fd = -1;
                    }
                }
            }
            clientQueue.timer = (clientQueue.timer + 1) % TIME_SLICE;
            lastTick += 100;
        }

        for (int i = 0; i < readyNum; ++i)
        {
            if(readyArr[i].data.fd == -1)
            {
                continue;
            }
            if (readyArr[i].data.fd == sockFd)// 监听到建立连接请求
            {
                // 监听socket也为非阻塞，每次事件最多接受32个连接
                for(int accepted = 0; accepted < 32; ++accepted)
                {
                    int netFd = accept(sockFd, NULL, NULL);
                    if(netFd == -1)
                    {
                        if(errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                        {
                            perror("accept");
                        }
                        break;
                    }
                    if((size_t)netFd >= clientQueue.fd_capacity || socket_nonblock(netFd) != 0)
                    {
                        close(netFd);
                        continue;
                    }
                    connection_t *pConnection = new(std::nothrow) connection_t{};
                    if(pConnection == NULL)
                    {
                        close(netFd);
                        continue;
                    }
                    pConnection->netFd = netFd;
                    pConnection->transfer_user_id = -1;
                    reset_request(pConnection);
                    clientQueue.connections[netFd].reset(pConnection);
                    clientQueue.client[netFd] = -1;
                    if(epollAdd(netFd, epfd) != 0)
                    {
                        release_connection(netFd, &clientQueue);
                        close(netFd);
                        continue;
                    }
                    if(fdAdd(netFd, &clientQueue) != 0)
                    {
                        close_client_connection(netFd, epfd, &clientQueue);
                    }
                }
            }
            else// 监听到客户端的消息或回复可以继续发送
            {
                int netFd = readyArr[i].data.fd;
                connection_t *pConnection = clientQueue.connections[netFd].get();
                if(pConnection == NULL)
                {
                    continue;
                }
                if(readyArr[i].events & (EPOLLERR | EPOLLHUP))
                {
                    close_client_connection(netFd, epfd, &clientQueue);
                    continue;
                }
                if(!pConnection->hasReply() && (readyArr[i].events & EPOLLIN))
                {
                    int progress = 0;
                    ret = receive_request(pConnection, &progress);
                    if(ret < 0 || (progress && fdAdd(netFd, &clientQueue) != 0))
                    {
                        close_invalid_connection(netFd, epfd, &clientQueue, "incomplete or invalid message");
                        continue;
                    }
                    if(ret == 1)
                    {
                        if(dispatch_request(pConnection, &clientQueue) != 0)
                        {
                            close_invalid_connection(netFd, epfd, &clientQueue, "command failed or is not allowed");
                            continue;
                        }
                        reset_request(pConnection);
                    }
                }
                if(pConnection->hasReply())
                {
                    ret = pConnection->flushReply();
                    if(ret < 0 || (ret > 0 && fdAdd(netFd, &clientQueue) != 0))
                    {
                        close_client_connection(netFd, epfd, &clientQueue);
                        continue;
                    }
                }
                // Token回复全部发出后才交接，工作线程不会与主线程同时收发
                if(!pConnection->hasReply() && pConnection->transfer_user_id >= 0)
                {
                    if(handoff_connection(netFd, epfd, &clientQueue, &threadPool) != 0)
                    {
                        close_client_connection(netFd, epfd, &clientQueue);
                    }
                    continue;
                }
                uint32_t events = !pConnection->hasReply() ? EPOLLIN : EPOLLOUT;
                if(epollMod(netFd, epfd, events) != 0)
                {
                    close_client_connection(netFd, epfd, &clientQueue);
                }
            }
        }
    }

    close(sockFd);// 不再接受新连接
    threadPool.stop();
    for(size_t i = 0; i < clientQueue.fd_capacity; ++i)
    {
        if(clientQueue.connections[i] != NULL)
        {
            close_client_connection((int)i, epfd, &clientQueue);
        }
    }
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGUSR1, SIG_IGN);
    close(exitPipe[0]);
    close(exitPipe[1]);
    close(epfd);
    return 0;
}

//初始化threadPoll结构体
int ThreadPool::initialize(int workerNum)
{
    threadNum = workerNum;
    try {
        tid.resize(workerNum);
    } catch(const std::bad_alloc &) {
        return -1;
    }
    //初始化锁和条件变量
    int ret = pthread_mutex_init(&taskQueue.mutex, NULL);
    if(ret != 0)
    {
        return -1;
    }
    ret = pthread_cond_init(&taskQueue.cond, NULL);
    if(ret != 0)
    {
        pthread_mutex_destroy(&taskQueue.mutex);
        return -1;
    }
    exitFlag = 0;
    return 0;
}
