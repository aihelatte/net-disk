#include "file_descriptor.h"
#include"threadPool.h"
#include"worker.h"
// 优雅的退出，完成任务再退出
void cleanFunc(void *arg)
{
    ThreadPool * pThreadPool = (ThreadPool*)arg;
    pthread_mutex_unlock(&pThreadPool->taskQueue.mutex);
}
//子线程函数入口
void *handleEvent(void *arg)
{
    ThreadPool * pThreadPool = (ThreadPool*)arg;
    TransferTask server;
    while(1)
    {
        // printf("I am free!tid = %lu\n", pthread_self());
        pthread_mutex_lock(&pThreadPool->taskQueue.mutex);
        pthread_cleanup_push(cleanFunc, (void*)pThreadPool);// 加锁后就push
        while(pThreadPool->taskQueue.empty() && pThreadPool->exitFlag == 0)// wait条件需要更改，否则唤醒后又会wait
        {
            pthread_cond_wait(&pThreadPool->taskQueue.cond, &pThreadPool->taskQueue.mutex);
        }
        if(pThreadPool->exitFlag != 0)
        {
            // printf("I am going to die!\n");
            pthread_exit(NULL);
        }
        server = pThreadPool->taskQueue.front();
        pThreadPool->taskQueue.pop();// 任务内容获取完成
        pthread_cleanup_pop(1);// 原来解锁的位置改为pop

        // printf("I am working! tid = %lu\n", pthread_self());

        handle_long_cmd(&server, pThreadPool);// 开始处理命令，建立新连接，用于长命令的发送

        pthread_mutex_lock(&pThreadPool->taskQueue.mutex);
        --pThreadPool->pendingTasks;// 成功、失败和超时均在返回后结束计数
        pthread_mutex_unlock(&pThreadPool->taskQueue.mutex);

        // printf("done!\n");
    }
}
int ThreadPool::createWorkers()
{
    for(int i = 0; i < threadNum; ++i)
    {
        int ret = pthread_create(&tid[i], NULL, handleEvent, this);
        if(ret != 0)
        {
            fprintf(stderr, "pthread_create: %s\n", strerror(ret));
            pthread_mutex_lock(&taskQueue.mutex);
            exitFlag = 1;
            pthread_cond_broadcast(&taskQueue.cond);
            pthread_mutex_unlock(&taskQueue.mutex);
            for(int j = 0; j < i; ++j)
            {
                pthread_join(tid[j], NULL);
            }
            close(tokenPipe[0]);
            close(tokenPipe[1]);
            pthread_cond_destroy(&tokenCond);
            pthread_cond_destroy(&taskQueue.cond);
            pthread_mutex_destroy(&taskQueue.mutex);
            return -1;
        }
    }
    running = true;
    return 0;
}
int handle_long_cmd(TransferTask *pserver, ThreadPool *pThreadPool)
{
    size_t cmdLength = strnlen(pserver->cmd, sizeof(pserver->cmd));
    if(cmdLength < 6 || cmdLength >= sizeof(pserver->cmd) ||
       (strncmp(pserver->cmd, "puts ", 5) != 0 && strncmp(pserver->cmd, "gets ", 5) != 0))
    {
        return EXIT_FAILURE;
    }
    char file[100] = {0};
    char md5_str[MD5_STR_LEN + 1] = {0};
    bool uploading = strncmp(pserver->cmd, "puts ", 5) == 0;
    if(uploading)
    {
        int i = 4;
        while(pserver->cmd[i] == ' ')
        {
            ++i;
        }
        if(cmdLength - i == 0 || cmdLength - i >= sizeof(file))
        {
            return EXIT_FAILURE;
        }
        strcpy(file, pserver->cmd + i);
        // 计算MD5和上传使用同一文件及锁，避免中间重新打开路径
    }
    FileDescriptor localFile(uploading ? open_in_root("upload", file, O_RDONLY, 0) : -1);
    int fileFd = localFile.get();
    if(uploading)
    {
        if(fileFd == -1)
        {
            perror("open");
            return EXIT_FAILURE;
        }
        if(flock(fileFd, LOCK_SH | LOCK_NB) == -1 ||
           Compute_fd_md5(fileFd, md5_str) != 0)
        {
            return EXIT_FAILURE;
        }
    }
    // puts("I am long cmd!");
    char token[PROTOCOL_TOKEN_CAPACITY];
    unsigned long tokenVersion = 0;
    if(get_token(pThreadPool, token, &tokenVersion, 0) != 0)
    {
        printf("Cannot obtain token, please login again.\n");
        return EXIT_FAILURE;
    }
    FileDescriptor connection(socket(AF_INET, SOCK_STREAM, 0));
    int sockFd_child = connection.get();
    if(sockFd_child == -1)
    {
        perror("socket");
        return EXIT_FAILURE;
    }
    if(tcpInit(&sockFd_child, "./config/client.conf") != 0)
    {
        return EXIT_FAILURE;
    }
    if(socket_nonblock(sockFd_child) != 0)
    {
        return EXIT_FAILURE;
    }
    // printf("Child connecting\n");
    int check = 0;
    int times = 0;
    while(1)
    {
        // 发送token验证消息
        train_state_t train_state;
        int length = snprintf(train_state.buf, sizeof(train_state.buf), "%s%s", pserver->user_name, token);
        if(length < 0 || length >= (int)sizeof(train_state.buf))
        {
            return EXIT_FAILURE;
        }
        train_state.state = TOKEN;
        train_state.length = strlen(train_state.buf);
        train_state.pre_length = strlen(pserver->user_name);
        if(wire_send_command(sockFd_child, &train_state, 0) != (int)(sizeof(train_state.length) + sizeof(train_state.state) + sizeof(train_state.pre_length) + train_state.length))
        {
            return EXIT_FAILURE;
        }
        if(wire_recv_int(sockFd_child, &check) != sizeof(check))
        {
            return -1;
        }
        if(check == TOKEN_VALID)
        {
            // 验证成功退出
            break;
        }
        if(check != TOKEN_EXPIRED || times != 0 ||
           get_token(pThreadPool, token, &tokenVersion, 1) != 0)
        {
            printf("Token authentication failed, please login again.\n");
            return EXIT_FAILURE;
        }
        ++times;// 只有明确过期才续签，最多重试一次
    }

    // 验证成功发送命令
    // printf("cmd = %s\n", pserver->cmd);
    train_t train;
    train.length = cmdLength;
    memcpy(train.buf, pserver->cmd, cmdLength);
    train.buf[cmdLength] = '\0';
    if(strncmp("puts ", train.buf, 5) != 0 && strncmp("gets ", train.buf, 5) != 0)
    {
        return EXIT_FAILURE;
    }
    if(wire_send_train(sockFd_child, &train, 0) != (int)(sizeof(train.length) + train.length))
    {
        return EXIT_FAILURE;
    }
    if(wire_send_int(sockFd_child, pserver->directoryId, MSG_NOSIGNAL) != sizeof(int))
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
