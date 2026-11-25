#include "file_descriptor.h"
#include"threadPool.h"
#include "worker.h"
// 优雅的退出，完成任务再退出
void cleanFunc(void *arg)
{
    ThreadPool * pThreadPool = (ThreadPool*)arg;
    pthread_mutex_unlock(&pThreadPool->taskQueue.mutex);
}

//线程池子线程入口函数
void *handleEvent(void *arg)
{
    ThreadPool * pThreadPool = (ThreadPool*)arg;
    int netFd;
    int user_id;
    while(1)
    {
        printf("I am free!tid = %lu\n", pthread_self());
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
        netFd = pThreadPool->taskQueue.front().netFd;
        user_id = pThreadPool->taskQueue.front().code;
        pThreadPool->taskQueue.pop();// 任务内容获取完成
        pthread_cleanup_pop(1);// 原来解锁的位置改为pop

        printf("I am working! tid = %lu\n", pthread_self());

        FileDescriptor connection(netFd);
        handle_long_cmd(connection.get(), user_id);// 开始处理命令，直接用netFd，监听客户端发来的命令，这个netFd是客户端子线程发来的

        printf("done!\n");
    }
}
//create线程
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
            pthread_cond_destroy(&taskQueue.cond);
            pthread_mutex_destroy(&taskQueue.mutex);
            return -1;
        }
    }
    running = true;
    return 0;
}
int handle_long_cmd(int netFd, int user_id)
{
    puts("I am long cmd!");
    // 接收命令
    char cmd[100] = {0};
    int datalength;
    if(wire_recv_int(netFd, &datalength) != sizeof(datalength) ||
       datalength < 6 || datalength >= (int)sizeof(cmd) ||
       recvn(netFd, cmd, datalength) != datalength)
    {
        return EXIT_FAILURE;
    }
    if(memchr(cmd, '\0', datalength) != NULL)
    {
        return EXIT_FAILURE;
    }
    int directoryId;
    if(wire_recv_int(netFd, &directoryId) != sizeof(int) || directoryId < 0)
    {
        return EXIT_FAILURE;
    }
    sysadd(user_id, cmd);
    
    // printf("cmd = %s\n", cmd);
    
    if (strncmp("puts ", cmd, 5) == 0)
    {
        return Download(netFd, user_id, directoryId);
    }
    else if(strncmp("gets ", cmd, 5) == 0)//gets
    {
        char file[100] = {0};
        int i = 4;
        while (cmd[i] == ' ')
        {
            ++i;
        }
        if(datalength - i <= 0 || datalength - i >= (int)sizeof(file))
        {
            return EXIT_FAILURE;
        }
        strcpy(file, cmd + i);
        // printf("file = %s\n", file);
        return Upload(netFd, file, user_id, directoryId);
    }
    else
    {
        return EXIT_FAILURE;
    }
    return 0;
}
