#include "threadPool.h"
#include "worker.h"
constexpr int workerCount = 3;
static volatile sig_atomic_t exitWriteFd = -1;
void sigFunc(int signum)
{
    int savedErrno = errno;
    while (write(exitWriteFd, "1", 1) == -1 && errno == EINTR)
    {
    }
    errno = savedErrno;
}
void sigFunc2(int signum)
{

}
// 登录后逐段读取命令，半行和超长行都能返回事件循环继续保活
static int read_command_line(char *buf, size_t capacity, int *length,
                             int *tooLong, int *extraCR)
{
    struct pollfd ready;
    ready.fd = STDIN_FILENO;
    ready.events = POLLIN;
    // 只有主线程读取stdin；每次最多处理1024字节，避免长输入独占主循环
    for (int i = 0; i < 1024; ++i)
    {
        ready.revents = 0;
        int available = poll(&ready, 1, 0);
        if (available == 0 || (available == -1 && errno == EINTR))
        {
            return 0;
        }
        if (available < 0 || (ready.revents & (POLLERR | POLLNVAL)))
        {
            return -1;
        }
        if (!(ready.revents & (POLLIN | POLLHUP)))
        {
            return 0;
        }
        char ch;
        ssize_t ret = read(STDIN_FILENO, &ch, 1);
        if (ret == -1 && errno == EINTR)
        {
            continue;
        }
        if (ret <= 0)
        {
            return -1;
        }
        if (ch == '\n')
        {
            int invalid = *tooLong;
            if (*length > 0 && buf[*length - 1] == '\r')
            {
                --*length;
            }
            buf[*length] = '\0';
            *length = 0;
            *tooLong = 0;
            *extraCR = 0;
            return invalid ? -2 : 1;
        }
        if (*extraCR)
        {
            *tooLong = 1;
        }
        if (ch == '\r' && (size_t)(*length + 1) == capacity && !*extraCR)
        {
            *extraCR = 1;
            continue;
        }
        if (ch == '\0' || (size_t)(*length + 1) >= capacity)
        {
            *tooLong = 1;
        }
        else
        {
            buf[(*length)++] = ch;
        }
    }
    return 0;
}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    int exitPipe[2];
    if (pipe(exitPipe) == -1)
    {
        return EXIT_FAILURE;
    }
    FileDescriptor exitRead(exitPipe[0]);
    FileDescriptor exitWrite(exitPipe[1]);
    if (socket_nonblock(exitWrite.get()) != 0)
    {
        return EXIT_FAILURE;
    }
    pid_t childPid = fork();
    if (childPid == -1)
    {
        return EXIT_FAILURE;
    }
    if (childPid != 0) // 父进程实现退出
    {
        //关闭读端
        exitRead.reset();
        exitWriteFd = exitWrite.get();
        signal(SIGUSR1, sigFunc);
        signal(SIGINT, sigFunc);
        //等待子进程终止
        while (waitpid(childPid, NULL, 0) == -1 && errno == EINTR)
        {
        }
        return 0;
    }
    //关闭写端
    exitWrite.reset();
    ThreadPool threadPool;
    FileDescriptor controlSocket(socket(AF_INET, SOCK_STREAM, 0));
    int sockFd = controlSocket.get();
    ERROR_CHECK(sockFd, -1, "socket");

    // 制作子线程
    //初始化线程池整个结构体
    if (threadPool.start(workerCount) != 0)
    {
        fprintf(stderr, "Failed to initialize thread pool.\n");
        return EXIT_FAILURE;
    }
    // 全部都等待唤醒


    // 主线程建立连接
    if (tcpInit(&sockFd, "./config/client.conf") != 0)
    {
        return EXIT_FAILURE;
    }
    printf("connecting...\n");

    int flag;
    char uName[100] = {0};

    // 整个登录注册的过程
    while (1)
    {
        flag = 0;
        printf("\033[1;34mPlease enter 1 to register a new user;");
        printf("or enter 2 to login: \033[0m");
        fflush(stdout);
        char buf[10] = {0};
        int inputLength = read_input_line(buf, sizeof(buf));
        if (inputLength == -1)
        {
            return EXIT_FAILURE;
        }
        if (inputLength == -2)
        {
            continue;
        }
        flag = atoi(buf);
        if (flag == 1)
        {
            int ret = register_user(sockFd);
            if (ret == -1)
            {
                return EXIT_FAILURE;
            }
            else if (ret == 1)
            {
                printf("\033[1;31mregister failed!\033[0m\n");
                continue;
            }
            else if (ret == 2)
            {
                printf("\033[1;33mUsername exist, register failed!\033[0m\n");
                continue;
            }
            else
            {
                printf("\033[1;32;46m注册成功，欢迎加入本网盘!!\033[0m\n");
                continue;
            }
        }
        else if (flag == 2)
        {
            int ret = login(sockFd, uName);
            if (ret == -1)
            {
                return EXIT_FAILURE;
            }
            else if (ret == 2)// 用户未注册
            {
                printf("\033[1;33mUnregistered user!\033[0m\n");
                continue;
            }
            else
            {
                printf("\033[1;32;46m%s你好，欢迎使用本网盘!!\033[0m\n", uName);
                break;
            }
        }
        else
        {
            printf("\033[1;34mmWrong num!,please try again!\033[0m\n");
            continue;
        }
    }
    signal(SIGINT, sigFunc2);// 登录成功后注册信号
    FileDescriptor epollFd(epoll_create(1));
    int epfd = epollFd.get();
    if (epfd == -1 || epollAdd(STDIN_FILENO, epfd) != 0 ||
       epollAdd(exitRead.get(), epfd) != 0 || epollAdd(sockFd, epfd) != 0 ||
       epollAdd(threadPool.tokenPipe[0].get(), epfd) != 0)
    {
        return EXIT_FAILURE;
    }
    struct epoll_event readyArr[4];

    // 只接收服务端签发的Token，签名密钥不再下发
    char token[1024] = {0};
    int64_t tokenDeadline;
    int datalength;
    if (receive_token(sockFd, token, &tokenDeadline) != 0)
    {
        return EXIT_FAILURE;
    }

    // 记录当前目录，刚登陆默认在家目录
    char pwd[1024] = {0};
    if (wire_recv_int(sockFd, &datalength) != sizeof(int) ||
       !protocol_valid_text_length(datalength, sizeof(pwd)) ||
       recvn(sockFd, pwd, datalength) != datalength)
    {
        return EXIT_FAILURE;
    }

    // 命令独立复制，Token单独共享并使用队列锁保护
    TransferTask server;
    bzero(&server, sizeof(server));
    {
        std::lock_guard<std::mutex> lock(threadPool.taskQueue.mutex);
        strcpy(threadPool.token, token);
        threadPool.tokenDeadline = tokenDeadline;
        threadPool.tokenVersion = 1;
        threadPool.tokenState = TokenState::ready;
    }
    strcpy(server.user_name, uName);

    // 开始接收命令，循环读取
    char cmd[PROTOCOL_COMMAND_CAPACITY] = {0};
    int inputLength = 0;
    int inputTooLong = 0;
    int inputExtraCR = 0;
    int printPrompt = 1;
    int64_t nextKeepalive = 0;
    train_state_t train_state;
    while (1)
    {
        int pendingTasks;
        {
            std::lock_guard<std::mutex> lock(threadPool.taskQueue.mutex);
            pendingTasks = threadPool.pendingTasks;
        }
        int waitTime = -1;
        if (pendingTasks > 0)
        {
            int64_t now = network_time_ms();
            if (now < 0)
            {
                return EXIT_FAILURE;
            }
            if (nextKeepalive == 0)
            {
                nextKeepalive = now + KEEPALIVE_INTERVAL_MS;
            }
            if (now >= nextKeepalive)
            {
                if (send_keepalive(sockFd) != 0)
                {
                    return EXIT_FAILURE;
                }
                now = network_time_ms();
                if (now < 0)
                {
                    return EXIT_FAILURE;
                }
                nextKeepalive = now + KEEPALIVE_INTERVAL_MS;
            }
            int64_t remaining = nextKeepalive - now;
            waitTime = remaining > 0 ? (int)remaining : 0;
        }
        else
        {
            nextKeepalive = 0;
        }
        if (printPrompt)
        {
            printf("\33[1m\33[34m[%s@ %s]: \33[0m", uName, pwd);
            fflush(stdout);
            printPrompt = 0;
        }
        int readyNum = epoll_wait(epfd, readyArr, 4, waitTime);
        if (readyNum == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return EXIT_FAILURE;
        }
        for (int i = 0; i < readyNum; ++i)
        {
            if (readyArr[i].data.fd == sockFd)// 服务端发送了超时断开要求
            {
                int check;
                if (wire_recv_int(sockFd, &check) != sizeof(check) || check != 1)
                {
                    controlSocket.reset();
                    threadPool.stop();
                    return EXIT_FAILURE;
                }
                printf("time out, bye!\n");

                train_t train;
                bzero(&train, sizeof(train));
                train.length = 0;
                wire_send_train(sockFd, &train, 0);
                threadPool.stop();
                return 0;
            }
            else if (readyArr[i].data.fd == threadPool.tokenPipe[0].get())
            {
                if (refresh_token(sockFd, &threadPool) != 0)
                {
                    printf("Token refresh failed, please login again.\n");
                    controlSocket.reset();
                    threadPool.stop();
                    return EXIT_FAILURE;
                }
            }
            else if (readyArr[i].data.fd == STDIN_FILENO)
            {
                if (inputLength == 0 && !inputTooLong && !inputExtraCR)
                {
                    bzero(cmd, sizeof(cmd));
                }
                int inputState = read_command_line(cmd, sizeof(cmd), &inputLength,
                                                   &inputTooLong, &inputExtraCR);
                if (inputState == 0)
                {
                    continue;
                }
                if (inputState == -1)
                {
                    return EXIT_FAILURE;
                }
                printPrompt = 1;
                if (inputState == -2)
                {
                    printf("\033[1;31mCommand too long!\033[0m\n");
                    continue;
                }
                bzero(&train_state, sizeof(train_state));
                if (strlen(cmd) < 2)
                {
                    printf("\033[1;31mwrong cmd!\033[0m\n");
                    continue;
                }
                if (strncmp("cd", cmd, 2) == 0)
                {
                    if (strlen(cmd) == 2 || cmd[2] != ' ' || cmd[2 + strspn(cmd + 2, " ")] == '\0')
                    {
                        printf("\033[1;31mwrong cmd!\033[0m\n");
                        continue;
                    }
                    train_state.state = CD;
                    strcpy(train_state.buf, cmd);
                    train_state.length = strlen(cmd);
                    train_state.pre_length = 2;
                    if (wire_send_command(sockFd, &train_state, 0) != (int)(sizeof(train_state.length) + sizeof(train_state.state) + sizeof(train_state.pre_length) + train_state.length))
                    {
                        return EXIT_FAILURE;
                    }
                    int check = 0;
                    if (wire_recv_int(sockFd, &check) != sizeof(int))
                    {
                        return EXIT_FAILURE;
                    }
                    if (check != 0)
                    {
                        printf("\033[1;31mWrong dir name!\033[0m\n");
                        continue;
                    }
                    if (wire_recv_int(sockFd, &datalength) != sizeof(int))
                    {
                        return EXIT_FAILURE;
                    }
                    bzero(pwd, sizeof(pwd));
                    if (!protocol_valid_text_length(datalength, sizeof(pwd)) || recvn(sockFd, pwd, datalength) != datalength)
                    {
                        return EXIT_FAILURE;
                    }
                }

                else if (strncmp("ls", cmd, 2) == 0)
                {
                    if (strlen(cmd) > 2 && cmd[2] != ' ')
                    {
                        printf("\033[1;31mwrong cmd!\033[0m\n");
                        continue;
                    }
                    train_state.state = LS;
                    strcpy(train_state.buf, cmd);
                    train_state.length = strlen(cmd);
                    train_state.pre_length = 2;
                    if (wire_send_command(sockFd, &train_state, 0) != (int)(sizeof(train_state.length) + sizeof(train_state.state) + sizeof(train_state.pre_length) + train_state.length))
                    {
                        return EXIT_FAILURE;
                    }
                    int check = 0;
                    if (wire_recv_int(sockFd, &check) != sizeof(int))
                    {
                        return EXIT_FAILURE;
                    }
                    if (check != 0)
                    {
                        printf("\033[1;31mWrong dir name!\033[0m\n");
                        continue;
                    }
                    char file_name[NAME_MAX + 1] = {0};
                    while (1)
                    {
                        if (wire_recv_int(sockFd, &datalength) != sizeof(datalength))
                        {
                            return EXIT_FAILURE;
                        }
                        if (datalength == 0)
                        {
                            break;
                        }
                        bzero(file_name, sizeof(file_name));
                        if (!protocol_valid_text_length(datalength, sizeof(file_name)) || recvn(sockFd, file_name, datalength) != datalength)
                        {
                            return EXIT_FAILURE;
                        }
                        printf("%-8s ", file_name);
                    }
                    printf("\n");
                }

                else if (strncmp("pwd", cmd, 3) == 0)
                {
                    if (strlen(cmd) > 3)
                    {
                        printf("\033[1;31mwrong cmd!\033[0m\n");
                        continue;
                    }
                    train_state.state = PWD;
                    strcpy(train_state.buf, cmd);
                    train_state.length = strlen(cmd);
                    train_state.pre_length = 3;
                    if (wire_send_command(sockFd, &train_state, 0) != (int)(sizeof(train_state.length) + sizeof(train_state.state) + sizeof(train_state.pre_length) + train_state.length))
                    {
                        return EXIT_FAILURE;
                    }
                    bzero(pwd, sizeof(pwd));
                    if (wire_recv_int(sockFd, &datalength) != sizeof(int) ||
                       !protocol_valid_text_length(datalength, sizeof(pwd)) ||
                       recvn(sockFd, pwd, datalength) != datalength)
                    {
                        return EXIT_FAILURE;
                    }
                    printf("%s\n", pwd);
                }

                else if (strncmp("rm", cmd, 2) == 0)
                {
                    if (strlen(cmd) == 2 || cmd[2] != ' ' || cmd[2 + strspn(cmd + 2, " ")] == '\0')
                    {
                        printf("\033[1;31mwrong cmd!\033[0m\n");
                        continue;
                    }
                    train_state.state = REMOVE;
                    strcpy(train_state.buf, cmd);
                    train_state.length = strlen(cmd);
                    train_state.pre_length = 2;
                    if (wire_send_command(sockFd, &train_state, 0) != (int)(sizeof(train_state.length) + sizeof(train_state.state) + sizeof(train_state.pre_length) + train_state.length))
                    {
                        return EXIT_FAILURE;
                    }
                    int check;
                    if (wire_recv_int(sockFd, &check) != sizeof(check))
                    {
                        return EXIT_FAILURE;
                    }
                    if (check != 0)
                    {
                        printf("\033[1;31mWrong file name!\033[0m\n");
                    }
                }

                else if (strncmp("mkdir", cmd, 5) == 0)
                {
                    if (strlen(cmd) == 5 || cmd[5] != ' ' || cmd[5 + strspn(cmd + 5, " ")] == '\0')
                    {
                        printf("\033[1;31mwrong cmd!\033[0m\n");
                        continue;
                    }
                    train_state.state = MKDIR;
                    strcpy(train_state.buf, cmd);
                    train_state.length = strlen(cmd);
                    train_state.pre_length = 5;
                    if (wire_send_command(sockFd, &train_state, 0) != (int)(sizeof(train_state.length) + sizeof(train_state.state) + sizeof(train_state.pre_length) + train_state.length))
                    {
                        return EXIT_FAILURE;
                    }
                    int check = 0;
                    if (wire_recv_int(sockFd, &check) != sizeof(int))
                    {
                        return EXIT_FAILURE;
                    }
                    if (check != 0)
                    {
                        printf("\033[1;31mCan not mkdir!\033[0m\n");
                        continue;
                    }
                }

                else if (strncmp("puts", cmd, 4) == 0 || strncmp("gets", cmd, 4) == 0)
                {
                    if (strlen(cmd) == 4 || strlen(cmd) >= sizeof(server.cmd) || cmd[4] != ' ' || cmd[4 + strspn(cmd + 4, " ")] == '\0')
                    {
                        printf("\033[1;31mwrong cmd!\033[0m\n");
                        continue;
                    }
                    // 控制连接返回提交命令时的目录编号，任务入队后独立保存。
                    train_state.state = cmd[0] == 'p' ? PUTS : GETS;
                    strcpy(train_state.buf, cmd);
                    train_state.length = strlen(cmd);
                    train_state.pre_length = 4;
                    if (wire_send_command(sockFd, &train_state, 0) != (int)(sizeof(train_state.length) + sizeof(train_state.state) + sizeof(train_state.pre_length) + train_state.length))
                    {
                        return EXIT_FAILURE;
                    }
                    if (wire_recv_int(sockFd, &server.directoryId) != sizeof(int) || server.directoryId < 0)
                    {
                        return EXIT_FAILURE;
                    }
                    strcpy(server.cmd, cmd);
                    std::lock_guard<std::mutex> lock(threadPool.taskQueue.mutex);
                    if (threadPool.pendingTasks == INT_MAX ||
                       threadPool.taskQueue.push(server) != 0)
                    {
                        printf("\033[1;31mTask enqueue failed!\033[0m\n");
                        continue;
                    }
                    ++threadPool.pendingTasks;
                    threadPool.taskQueue.cond.notify_one(); // 唤醒子线程
                }

                else
                {
                    printf("\033[1;31mwrong cmd!\033[0m\n");
                    continue;
                }
            }
            else// 退出
            {
                // 等待活跃任务结束后再通知服务端关闭控制连接
                threadPool.stop();
                train_t train;
                bzero(&train, sizeof(train));
                train.length = 0;
                wire_send_train(sockFd, &train, 0);
                return 0;
            }
        }
    }
    return 0;
}
