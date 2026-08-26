#include "threadPool.h"
#include "worker.h"
#include "protocol_validate.h"
#define NUM 3       //线程池中子线程数量，固定为3

int exitPipe[2];

static int is_valid_train_header(const train_state_t *pTrain)
{
    if(!protocol_valid_text_length(pTrain->length, sizeof(pTrain->buf)) ||
       pTrain->state < REGISTER || pTrain->state > TOKEN ||
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
    default:
        return 0;
    }
}

static int is_valid_train_body(const train_state_t *pTrain)
{
    switch(pTrain->state)
    {
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

static void close_invalid_connection(int netFd, int epfd, const char *reason)
{
    fprintf(stderr, "Invalid protocol message: fd=%d, reason=%s\n", netFd, reason);
    epollDel(netFd, epfd);
    close(netFd);
}
void sigFunc(int signum)
{
    // printf("signum = %d\n", signum);
    write(exitPipe[1], "1", 1);
    // printf("Parent process is going to die!\n");
}
void sigFunc2(int signum)
{
    
}
int main(int argc, char *argv[])
{
    //管理一个客户端断开的管道
    pipe(exitPipe);
    //if (fork() != 0) // 父进程实现退出
    //{
    //    close(exitPipe[0]);
    //    signal(SIGUSR1, sigFunc);
    //    signal(SIGINT, sigFunc);
    //    wait(NULL);
    //    exit(0);
    //}
    //signal(SIGINT, sigFunc2);
    //close(exitPipe[1]);
    // 制作子线程
    int workerNum = NUM;
    threadPool_t threadPool;
    threadPoolInit(&threadPool, workerNum);
    makeWorker(&threadPool);
    
    // 绑定ip和端口号
    int sockFd;
    tcpInit(&sockFd, "./config/server.conf"); // 以读取config文件的形式建立连接

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
    init_local_file();

    // 开始监听
    epollAdd(sockFd, epfd);// 监听连接请求
    epollAdd(exitPipe[0], epfd); // 监听退出请求
    struct epoll_event readyArr[3];
    int ret;
    time_t timeStart, timeEnd;
    while (1)
    {
        // printf("epoll_wait!\n");
        int readyNum = epoll_wait(epfd, readyArr, 3, 100);// 每0.1s醒来一次，对已连接的客户端等待时间-1
        // printf("epoll_wait ready!\n");

        // 当前无就绪事件，查看集合内是否是否有连接需要超时断开
        if(readyNum == 0)
        {
            if(clientQueue.time_out[clientQueue.timer].size != 0)
            {
                slotNode_t *cur = clientQueue.time_out[clientQueue.timer].head;
                int netFd;
                while(cur != NULL)// 队列不为空则关闭队列中的所有netFd
                {
                    netFd = cur->netFd;
                    send(netFd, "1", 1, MSG_NOSIGNAL);
                    cur = cur->next;
                    // fdDel(netFd, &clientQueue);// 在退出那里删了，就不需要重复删了
                }
            }
            clientQueue.timer = (clientQueue.timer + 1) % TIME_SLICE;
        }

        for (int i = 0; i < readyNum; ++i)
        {
            if (readyArr[i].data.fd == sockFd)// 监听到建立连接请求
            {
                int netFd = accept(sockFd, NULL, NULL);
                if(netFd == -1)
                {
                    perror("accept");
                    continue;
                }
                if((size_t)netFd >= clientQueue.fd_capacity)
                {
                    fprintf(stderr, "File descriptor is out of range: fd=%d\n", netFd);
                    close(netFd);
                    continue;
                }
                // 将连接加入监听
                epollAdd(netFd, epfd);
            }
            else if (readyArr[i].data.fd == exitPipe[0]) // 监听到退出
            {
                // printf("child process, threadPool is going to die!\n");
                threadPool.exitFlag = 1;
                pthread_cond_broadcast(&threadPool.taskQueue.cond); // 叫醒所有线程
                for (int j = 0; j < workerNum; ++j)
                {
                    pthread_join(threadPool.tid[j], NULL); // 等待子线程终结
                }
                pthread_exit(NULL);
            }
            else// 监听到客户端的消息
            {
                int netFd = readyArr[i].data.fd;
                printf("netFd = %d\n", netFd);
                
                //命令版tlv协议数据单元
                train_state_t train_get;
                bzero(&train_get, sizeof(train_get));
                ret = recv(netFd, &train_get.length, sizeof(int), 0);
                
                // printf("datalength = %d, ret = %d\n", train_get.length, ret);
                if(ret == 0 || train_get.length == 0)// 发现客户端退出了
                {
                    char uName[100] = {0};
                    get_user_name(uName,clientQueue.client[netFd]);//netfd-->user_id-->user_name
                    printf("User %s leave!\n", uName);
                    fdDel(netFd, &clientQueue);// 把该netFd从轮盘中删除
                    update_user_pwd(clientQueue.client[netFd], "~/", 0);// 退出的时候客户退回根目录
                    clientQueue.client[netFd] = -1;     //哈希槽填-1
                    epollDel(netFd, epfd);// 从监听中去除
                    // clientDequeue(&clientQueue, netFd);// 客户端从队列中删除
                    close(netFd);
                    break;
                }
                if(!protocol_valid_text_length(train_get.length, sizeof(train_get.buf)))
                {
                    close_invalid_connection(netFd, epfd, "invalid length");
                    continue;
                }
                if(recvn(netFd, &train_get.state, sizeof(int)) != sizeof(int) ||
                   recvn(netFd, &train_get.pre_length, sizeof(int)) != sizeof(int))
                {
                    close_invalid_connection(netFd, epfd, "incomplete message header");
                    continue;
                }
                if(!is_valid_train_header(&train_get))
                {
                    close_invalid_connection(netFd, epfd, "invalid header fields");
                    continue;
                }
                if(recvn(netFd, train_get.buf, train_get.length) != train_get.length)
                {
                    close_invalid_connection(netFd, epfd, "incomplete message body");
                    continue;
                }
                train_get.buf[train_get.length] = '\0';
                if(!is_valid_train_body(&train_get))
                {
                    close_invalid_connection(netFd, epfd, "state and body mismatch");
                    continue;
                }
                // printf("train.buf = %s\n", train_get.buf);

                switch(train_get.state)
                {
                case REGISTER:
                    register_user(train_get, netFd);
                    break;
                case LOGIN:
                    ret = login(train_get, netFd, &clientQueue);
                    if(ret == 0)
                    {
                        fdAdd(netFd, &clientQueue);// 登录成功开始计时
                    }
                    // clientQueue.time_out[netFd] = 5;// 登录成功后也需要重置等待时间
                    break;
                case LS:
                case CD:
                case PWD:
                case REMOVE:
                case MKDIR:
                    // 收到客户端发来的命令，先删掉再添加，重置时间
                    fdDel(netFd, &clientQueue);
                    fdAdd(netFd, &clientQueue);
                    handle_short_cmd(train_get, netFd, clientQueue.client[netFd]);
                    break;
                case PUTS:
                case GETS:
                    fdDel(netFd, &clientQueue);
                    fdAdd(netFd, &clientQueue);
                    break;
                case TOKEN:
                    // 验证token值，成功返回对应客户端在队列中的位置
                    //返回值同时也提供user_id
                    ret = token(train_get, netFd);
                    if(ret != -1)// 验证成功，唤醒子线程
                    {
                        //这是客户端的子线程数据连接，服务端主线程不再监听
                        epollDel(netFd, epfd);
                        pthread_mutex_lock(&threadPool.taskQueue.mutex);
                        taskEnqueue(&threadPool.taskQueue, ret, netFd);// 把user_id 和 netFd 传给子线程
                        printf("Time to work!\n");
                        //可以唤醒所有队列
                        pthread_cond_signal(&threadPool.taskQueue.cond);
                        pthread_mutex_unlock(&threadPool.taskQueue.mutex);
                    }
                default:
                    break;
                }
            }
        }
    }
}

//初始化threadPoll结构体
int threadPoolInit(threadPool_t *pThreadPool, int workerNum)    
{
    pThreadPool->threadNum = workerNum;
    pThreadPool->tid = (pthread_t *)calloc(workerNum, sizeof(pthread_t));
    pThreadPool->taskQueue.pFront = NULL;
    pThreadPool->taskQueue.pRear = NULL;
    pThreadPool->taskQueue.size = 0;
    //初始化锁和条件变量
    pthread_mutex_init(&pThreadPool->taskQueue.mutex, NULL);
    pthread_cond_init(&pThreadPool->taskQueue.cond, NULL);
    pThreadPool->exitFlag = 0;
}
