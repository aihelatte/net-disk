#include "threadPool.h"
#include "worker.h"
#define NUM 3
int exitPipe[2];
void sigFunc(int signum)
{
    int savedErrno = errno;
    // printf("signum = %d\n", signum);
    while(write(exitPipe[1], "1", 1) == -1 && errno == EINTR)
    {
    }
    errno = savedErrno;
    // printf("Parent process is going to die!\n");
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
    for(int i = 0; i < 1024; ++i)
    {
        ready.revents = 0;
        int available = poll(&ready, 1, 0);
        if(available == 0 || (available == -1 && errno == EINTR))
        {
            return 0;
        }
        if(available < 0 || (ready.revents & (POLLERR | POLLNVAL)))
        {
            return -1;
        }
        if(!(ready.revents & (POLLIN | POLLHUP)))
        {
            return 0;
        }
        char ch;
        ssize_t ret = read(STDIN_FILENO, &ch, 1);
        if(ret == -1 && errno == EINTR)
        {
            continue;
        }
        if(ret <= 0)
        {
            return -1;
        }
        if(ch == '\n')
        {
            int invalid = *tooLong;
            if(*length > 0 && buf[*length - 1] == '\r')
            {
                --*length;
            }
            buf[*length] = '\0';
            *length = 0;
            *tooLong = 0;
            *extraCR = 0;
            return invalid ? -2 : 1;
        }
        if(*extraCR)
        {
            *tooLong = 1;
        }
        if(ch == '\r' && (size_t)(*length + 1) == capacity && !*extraCR)
        {
            *extraCR = 1;
            continue;
        }
        if(ch == '\0' || (size_t)(*length + 1) >= capacity)
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

static void stop_workers(threadPool_t *pThreadPool)
{
    pthread_mutex_lock(&pThreadPool->taskQueue.mutex);
    pThreadPool->exitFlag = 1;
    while(pThreadPool->taskQueue.size > 0)
    {
        taskDequeue(&pThreadPool->taskQueue);
        --pThreadPool->pendingTasks;
    }
    pthread_cond_broadcast(&pThreadPool->taskQueue.cond);
    pthread_mutex_unlock(&pThreadPool->taskQueue.mutex);
    close_token(pThreadPool);// 唤醒等待续签的线程，避免退出时卡在join
    for(int i = 0; i < pThreadPool->threadNum; ++i)
    {
        pthread_join(pThreadPool->tid[i], NULL);
    }
    free(pThreadPool->tid);
    close(pThreadPool->tokenPipe[0]);
    close(pThreadPool->tokenPipe[1]);
    pthread_cond_destroy(&pThreadPool->tokenCond);
    pthread_cond_destroy(&pThreadPool->taskQueue.cond);
    pthread_mutex_destroy(&pThreadPool->taskQueue.mutex);
}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    if(pipe(exitPipe) == -1)
    {
        return EXIT_FAILURE;
    }
    if(socket_nonblock(exitPipe[1]) != 0)
    {
        close(exitPipe[0]);
        close(exitPipe[1]);
        return EXIT_FAILURE;
    }
    pid_t childPid = fork();
    if(childPid == -1)
    {
        close(exitPipe[0]);
        close(exitPipe[1]);
        return EXIT_FAILURE;
    }
    if (childPid != 0) // 父进程实现退出
    {
        //关闭读端
        close(exitPipe[0]);
        signal(SIGUSR1, sigFunc);
        signal(SIGINT, sigFunc);
        //等待子进程终止
        while(waitpid(childPid, NULL, 0) == -1 && errno == EINTR)
        {
        }
        close(exitPipe[1]);
        exit(0);
    }
    //关闭写端
    close(exitPipe[1]);
    int sockFd = socket(AF_INET, SOCK_STREAM, 0);
    ERROR_CHECK(sockFd, -1, "socket");

    // 制作子线程
    int workerNum = NUM;
    threadPool_t threadPool;
    //初始化线程池整个结构体
    if(threadPoolInit(&threadPool, workerNum) != 0 || makeWorker(&threadPool) != 0)
    {
        fprintf(stderr, "Failed to initialize thread pool.\n");
        return EXIT_FAILURE;
    }
    // 全部都等待唤醒


    // 主线程建立连接
    if(tcpInit(&sockFd, "./config/client.conf") != 0)
    {
        close(sockFd);
        return EXIT_FAILURE;
    }
    printf("connecting...\n");

    int flag;
    char uName[100] = {0};
    
    // 整个登录注册的过程
    while(1)
    {
        flag = 0;
        printf("\033[1;34mPlease enter 1 to register a new user;");
        printf("or enter 2 to login: \033[0m");
        fflush(stdout);
        char buf[10] = {0};
        int inputLength = read_input_line(buf, sizeof(buf));
        if(inputLength == -1)
        {
            close(sockFd);
            return EXIT_FAILURE;
        }
        if(inputLength == -2)
        {
            continue;
        }
        // read(STDIN_FILENO, &flag, sizeof(int));
        // printf("buf = %s\n", buf);
        flag = atoi(buf);
        // scanf("%d", &flag);// 用scanf如果输入字母会死循环
        // printf("flag = %d\n", flag);
        if(flag == 1)
        {
            int ret = register_user(sockFd);
            if(ret == -1)
            {
                close(sockFd);
                return EXIT_FAILURE;
            }
            else if(ret == 1)
            {
                printf("\033[1;31mregister failed!\033[0m\n");
                continue;
            }
            else if(ret == 2)
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
        else if(flag == 2)
        {
            int ret = login(sockFd, uName);
            if(ret == -1)
            {
                close(sockFd);
                return EXIT_FAILURE;
            }
            else if(ret == 2)// 用户未注册
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
    int epfd = epoll_create(1);
    if(epfd == -1 || epollAdd(STDIN_FILENO, epfd) != 0 ||
       epollAdd(exitPipe[0], epfd) != 0 || epollAdd(sockFd, epfd) != 0 ||
       epollAdd(threadPool.tokenPipe[0], epfd) != 0)
    {
        close(sockFd);
        if(epfd != -1)
        {
            close(epfd);
        }
        return EXIT_FAILURE;
    }
    struct epoll_event readyArr[4];

    // 只接收服务端签发的Token，签名密钥不再下发
    char token[1024] = {0};
    int64_t tokenDeadline;
    int datalength;
    if(receive_token(sockFd, token, &tokenDeadline) != 0)
    {
        close(sockFd);
        return EXIT_FAILURE;
    }

    // 记录当前目录，刚登陆默认在家目录
    char pwd[1024] = {0};
    if(wire_recv_int(sockFd, &datalength) != sizeof(int) ||
       !protocol_valid_text_length(datalength, sizeof(pwd)) ||
       recvn(sockFd, pwd, datalength) != datalength)
    {
        close(sockFd);
        return EXIT_FAILURE;
    }

    // 命令独立复制，Token单独共享并使用队列锁保护
    server_t server;
    bzero(&server, sizeof(server));
    pthread_mutex_lock(&threadPool.taskQueue.mutex);
    strcpy(threadPool.token, token);
    threadPool.tokenDeadline = tokenDeadline;
    threadPool.tokenVersion = 1;
    threadPool.tokenState = TOKEN_READY;
    pthread_mutex_unlock(&threadPool.taskQueue.mutex);
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
        pthread_mutex_lock(&threadPool.taskQueue.mutex);
        int pendingTasks = threadPool.pendingTasks;
        pthread_mutex_unlock(&threadPool.taskQueue.mutex);
        int waitTime = -1;
        if(pendingTasks > 0)
        {
            int64_t now = network_time_ms();
            if(now < 0)
            {
                close(sockFd);
                close(epfd);
                return EXIT_FAILURE;
            }
            if(nextKeepalive == 0)
            {
                nextKeepalive = now + KEEPALIVE_INTERVAL_MS;
            }
            if(now >= nextKeepalive)
            {
                if(send_keepalive(sockFd) != 0)
                {
                    close(sockFd);
                    close(epfd);
                    return EXIT_FAILURE;
                }
                now = network_time_ms();
                if(now < 0)
                {
                    close(sockFd);
                    close(epfd);
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
        if(printPrompt)
        {
            printf("\33[1m\33[34m[%s@ %s]: \33[0m", uName, pwd);
            fflush(stdout);
            printPrompt = 0;
        }
        int readyNum = epoll_wait(epfd, readyArr, 4, waitTime);
        if(readyNum == -1)
        {
            if(errno == EINTR)
            {
                continue;
            }
            close(sockFd);
            close(epfd);
            return EXIT_FAILURE;
        }
        // printf("epoll_wait ready!\n");
        for (int i = 0; i < readyNum; ++i)
        {
            if(readyArr[i].data.fd == sockFd)// 服务端发送了超时断开要求
            {
                int check;
                if(wire_recv_int(sockFd, &check) != sizeof(check) || check != 1)
                {
                    close(sockFd);
                    stop_workers(&threadPool);
                    close(epfd);
                    close(exitPipe[0]);
                    return EXIT_FAILURE;
                }
                printf("time out, bye!\n");

                train_t train;
                bzero(&train, sizeof(train));
                train.length = 0;
                wire_send_train(sockFd, &train, 0);
                // printf("child process, threadPool is going to die!\n");
                stop_workers(&threadPool);
                close(sockFd);
                close(epfd);
                close(exitPipe[0]);
                return 0;
            }
            else if(readyArr[i].data.fd == threadPool.tokenPipe[0])
            {
                if(refresh_token(sockFd, &threadPool) != 0)
                {
                    printf("Token refresh failed, please login again.\n");
                    close(sockFd);
                    stop_workers(&threadPool);
                    close(epfd);
                    close(exitPipe[0]);
                    return EXIT_FAILURE;
                }
            }
            else if (readyArr[i].data.fd == STDIN_FILENO)
            {
                if(inputLength == 0 && !inputTooLong && !inputExtraCR)
                {
                    bzero(cmd, sizeof(cmd));
                }
                int inputState = read_command_line(cmd, sizeof(cmd), &inputLength,
                                                   &inputTooLong, &inputExtraCR);
                if(inputState == 0)
                {
                    continue;
                }
                if(inputState == -1)
                {
                    close(sockFd);
                    return EXIT_FAILURE;
                }
                printPrompt = 1;
                if(inputState == -2)
                {
                    printf("\033[1;31mCommand too long!\033[0m\n");
                    continue;
                }
                //printf("cmd = %s\n", cmd);
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
                    if(wire_send_command(sockFd, &train_state, 0) != (int)(sizeof(train_state.length) + sizeof(train_state.state) + sizeof(train_state.pre_length) + train_state.length))
                    {
                        close(sockFd);
                        return EXIT_FAILURE;
                    }
                    int check = 0;
                    if(wire_recv_int(sockFd, &check) != sizeof(int))
                    {
                        close(sockFd);
                        return EXIT_FAILURE;
                    }
                    if (check != 0)
                    {
                        printf("\033[1;31mWrong dir name!\033[0m\n");
                        continue;
                    }
                    if(wire_recv_int(sockFd, &datalength) != sizeof(int))
                    {
                        close(sockFd);
                        return EXIT_FAILURE;
                    }
                    bzero(pwd, sizeof(pwd));
                    if(!protocol_valid_text_length(datalength, sizeof(pwd)) || recvn(sockFd, pwd, datalength) != datalength)
                    {
                        close(sockFd);
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
                    if(wire_send_command(sockFd, &train_state, 0) != (int)(sizeof(train_state.length) + sizeof(train_state.state) + sizeof(train_state.pre_length) + train_state.length))
                    {
                        close(sockFd);
                        return EXIT_FAILURE;
                    }
                    int check = 0;
                    if(wire_recv_int(sockFd, &check) != sizeof(int))
                    {
                        close(sockFd);
                        return EXIT_FAILURE;
                    }
                    //printf("check = %d\n", check);
                    if (check != 0)
                    {
                        printf("\033[1;31mWrong dir name!\033[0m\n");
                        continue;
                    }
                    char file_name[NAME_MAX + 1] = {0};
                    while(1)
                    {
                        if(wire_recv_int(sockFd, &datalength) != sizeof(datalength))
                        {
                            close(sockFd);
                            return EXIT_FAILURE;
                        }
                        if(datalength == 0)
                        {
                            break;
                        }
                        bzero(file_name, sizeof(file_name));
                        if(!protocol_valid_text_length(datalength, sizeof(file_name)) || recvn(sockFd, file_name, datalength) != datalength)
                        {
                            close(sockFd);
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
                    if(wire_send_command(sockFd, &train_state, 0) != (int)(sizeof(train_state.length) + sizeof(train_state.state) + sizeof(train_state.pre_length) + train_state.length))
                    {
                        close(sockFd);
                        return EXIT_FAILURE;
                    }
                    bzero(pwd, sizeof(pwd));
                    if(wire_recv_int(sockFd, &datalength) != sizeof(int) ||
                       !protocol_valid_text_length(datalength, sizeof(pwd)) ||
                       recvn(sockFd, pwd, datalength) != datalength)
                    {
                        close(sockFd);
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
                    if(wire_send_command(sockFd, &train_state, 0) != (int)(sizeof(train_state.length) + sizeof(train_state.state) + sizeof(train_state.pre_length) + train_state.length))
                    {
                        close(sockFd);
                        return EXIT_FAILURE;
                    }
                    int check;
                    if(wire_recv_int(sockFd, &check) != sizeof(check))
                    {
                        close(sockFd);
                        return EXIT_FAILURE;
                    }
                    if(check != 0)
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
                    if(wire_send_command(sockFd, &train_state, 0) != (int)(sizeof(train_state.length) + sizeof(train_state.state) + sizeof(train_state.pre_length) + train_state.length))
                    {
                        close(sockFd);
                        return EXIT_FAILURE;
                    }
                    int check = 0;
                    if(wire_recv_int(sockFd, &check) != sizeof(int))
                    {
                        close(sockFd);
                        return EXIT_FAILURE;
                    }
                    if (check != 0)
                    {
                        printf("\033[1;31mCan not mkdir!\033[0m\n");
                        continue;
                    }
                }

                else if (strncmp("puts", cmd, 4) == 0)
                {
                    if (strlen(cmd) == 4 || strlen(cmd) >= sizeof(server.cmd) || cmd[4] != ' ' || cmd[4 + strspn(cmd + 4, " ")] == '\0')
                    {
                        printf("\033[1;31mwrong cmd!\033[0m\n");
                        continue;
                    }
                    // 先发送命令重置一下时间
                    train_state.state = PUTS;
                    strcpy(train_state.buf, cmd);
                    train_state.length = strlen(cmd);
                    train_state.pre_length = 4;
                    if(wire_send_command(sockFd, &train_state, 0) != (int)(sizeof(train_state.length) + sizeof(train_state.state) + sizeof(train_state.pre_length) + train_state.length))
                    {
                        close(sockFd);
                        return EXIT_FAILURE;
                    }
                    // 唤醒子线程
                    strcpy(server.cmd, cmd);
                    pthread_mutex_lock(&threadPool.taskQueue.mutex);
                    if(threadPool.pendingTasks == INT_MAX ||
                       taskEnqueue(&threadPool.taskQueue, &server) != 0)
                    {
                        pthread_mutex_unlock(&threadPool.taskQueue.mutex);
                        printf("\033[1;31mTask enqueue failed!\033[0m\n");
                        continue;
                    }
                    ++threadPool.pendingTasks;
                    pthread_cond_signal(&threadPool.taskQueue.cond); // 唤醒子线程
                    pthread_mutex_unlock(&threadPool.taskQueue.mutex);
                }

                else if (strncmp("gets", cmd, 4) == 0)
                {
                    if (strlen(cmd) == 4 || strlen(cmd) >= sizeof(server.cmd) || cmd[4] != ' ' || cmd[4 + strspn(cmd + 4, " ")] == '\0')
                    {
                        printf("\033[1;31mwrong cmd!\033[0m\n");
                        continue;
                    }
                    // 先发送命令重置一下时间
                    train_state.state = GETS;
                    strcpy(train_state.buf, cmd);
                    train_state.length = strlen(cmd);
                    train_state.pre_length = 4;
                    if(wire_send_command(sockFd, &train_state, 0) != (int)(sizeof(train_state.length) + sizeof(train_state.state) + sizeof(train_state.pre_length) + train_state.length))
                    {
                        close(sockFd);
                        return EXIT_FAILURE;
                    }
                    // 唤醒子线程
                    strcpy(server.cmd, cmd);
                    pthread_mutex_lock(&threadPool.taskQueue.mutex);
                    if(threadPool.pendingTasks == INT_MAX ||
                       taskEnqueue(&threadPool.taskQueue, &server) != 0)
                    {
                        pthread_mutex_unlock(&threadPool.taskQueue.mutex);
                        printf("\033[1;31mTask enqueue failed!\033[0m\n");
                        continue;
                    }
                    ++threadPool.pendingTasks;
                    pthread_cond_signal(&threadPool.taskQueue.cond); // 唤醒子线程
                    pthread_mutex_unlock(&threadPool.taskQueue.mutex);
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
                stop_workers(&threadPool);
                train_t train;
                bzero(&train, sizeof(train));
                train.length = 0;
                wire_send_train(sockFd, &train, 0);
                close(sockFd);
                close(epfd);
                close(exitPipe[0]);
                return 0;
            }
        }
    }
    close(sockFd);
    return 0;
}
int threadPoolInit(threadPool_t *pThreadPool, int workerNum)
{
    pThreadPool->threadNum = workerNum;
    pThreadPool->tid = (pthread_t *)calloc(workerNum, sizeof(pthread_t));
    if(pThreadPool->tid == NULL)
    {
        return -1;
    }
    pThreadPool->taskQueue.pFront = NULL;
    pThreadPool->taskQueue.pRear = NULL;
    pThreadPool->taskQueue.size = 0;
    int ret = pthread_mutex_init(&pThreadPool->taskQueue.mutex, NULL);
    if(ret != 0)
    {
        free(pThreadPool->tid);
        return -1;
    }
    ret = pthread_cond_init(&pThreadPool->taskQueue.cond, NULL);
    if(ret != 0)
    {
        pthread_mutex_destroy(&pThreadPool->taskQueue.mutex);
        free(pThreadPool->tid);
        return -1;
    }
    pThreadPool->exitFlag = 0;
    pThreadPool->pendingTasks = 0;
    pThreadPool->tokenState = TOKEN_CLOSED;
    pThreadPool->tokenVersion = 0;
    pThreadPool->tokenDeadline = 0;
    pThreadPool->token[0] = '\0';
    pthread_condattr_t attr;
    ret = pthread_condattr_init(&attr);
    if(ret == 0)
    {
        ret = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        if(ret == 0)
        {
            ret = pthread_cond_init(&pThreadPool->tokenCond, &attr);
        }
        pthread_condattr_destroy(&attr);
    }
    if(ret != 0)
    {
        pthread_cond_destroy(&pThreadPool->taskQueue.cond);
        pthread_mutex_destroy(&pThreadPool->taskQueue.mutex);
        free(pThreadPool->tid);
        return -1;
    }
    if(pipe(pThreadPool->tokenPipe) == -1)
    {
        pthread_cond_destroy(&pThreadPool->tokenCond);
        pthread_cond_destroy(&pThreadPool->taskQueue.cond);
        pthread_mutex_destroy(&pThreadPool->taskQueue.mutex);
        free(pThreadPool->tid);
        return -1;
    }
    if(socket_nonblock(pThreadPool->tokenPipe[0]) != 0 ||
       socket_nonblock(pThreadPool->tokenPipe[1]) != 0)
    {
        close(pThreadPool->tokenPipe[0]);
        close(pThreadPool->tokenPipe[1]);
        pthread_cond_destroy(&pThreadPool->tokenCond);
        pthread_cond_destroy(&pThreadPool->taskQueue.cond);
        pthread_mutex_destroy(&pThreadPool->taskQueue.mutex);
        free(pThreadPool->tid);
        return -1;
    }
    return 0;
}
