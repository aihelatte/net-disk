#include "threadPool.h"
#include "worker.h"

int receive_token(int sockFd, char *token, int64_t *pDeadline)
{
    int length;
    int lifetime;
    int64_t now = network_time_ms();
    if(now < 0 || wire_recv_int(sockFd, &length) != sizeof(length) ||
       length <= 0 || !protocol_valid_text_length(length, PROTOCOL_TOKEN_CAPACITY) ||
       recvn(sockFd, token, length) != length ||
       memchr(token, '\0', length) != NULL ||
       wire_recv_int(sockFd, &lifetime) != sizeof(lifetime) ||
       lifetime != TOKEN_LIFETIME)
    {
        return -1;
    }
    token[length] = '\0';
    *pDeadline = now + (int64_t)lifetime * 1000;// 仅用于提前续签，服务端负责最终有效期校验
    return 0;
}

void close_token(ThreadPool *pThreadPool)
{
    pthread_mutex_lock(&pThreadPool->taskQueue.mutex);
    pThreadPool->tokenState = TOKEN_CLOSED;
    pthread_cond_broadcast(&pThreadPool->tokenCond);
    pthread_mutex_unlock(&pThreadPool->taskQueue.mutex);
}

int get_token(ThreadPool *pThreadPool, char *token,
              unsigned long *pVersion, int forceRefresh)
{
    int64_t now = network_time_ms();
    if(now < 0)
    {
        return -1;
    }
    int64_t deadline = now + NETWORK_IDLE_TIMEOUT_MS;
    struct timespec timeout;
    timeout.tv_sec = deadline / 1000;
    timeout.tv_nsec = (deadline % 1000) * 1000000;

    pthread_mutex_lock(&pThreadPool->taskQueue.mutex);
    while(pThreadPool->tokenState != TOKEN_CLOSED)
    {
        if(forceRefresh && *pVersion != pThreadPool->tokenVersion)
        {
            forceRefresh = 0;// 每次唤醒后重查，避免同一代凭证重复续签
        }
        now = network_time_ms();
        if(now < 0)
        {
            break;
        }
        if(pThreadPool->tokenState == TOKEN_READY && !forceRefresh &&
           now + TOKEN_REFRESH_MARGIN_MS < pThreadPool->tokenDeadline)
        {
            strcpy(token, pThreadPool->token);
            *pVersion = pThreadPool->tokenVersion;
            pthread_mutex_unlock(&pThreadPool->taskQueue.mutex);
            return 0;
        }
        if(pThreadPool->tokenState == TOKEN_READY)
        {
            pThreadPool->tokenState = TOKEN_REQUESTED;
            pthread_mutex_unlock(&pThreadPool->taskQueue.mutex);
            ssize_t ret;
            do
            {
                ret = write(pThreadPool->tokenPipe[1], "1", 1);
            }while(ret == -1 && errno == EINTR);
            int notifyFailed = ret != 1 && !(ret == -1 && errno == EAGAIN);
            pthread_mutex_lock(&pThreadPool->taskQueue.mutex);
            if(notifyFailed)
            {
                break;
            }
            forceRefresh = 0;
            continue;
        }
        int ret = pthread_cond_timedwait(&pThreadPool->tokenCond,
                                         &pThreadPool->taskQueue.mutex, &timeout);
        if(ret != 0)
        {
            break;
        }
    }
    pThreadPool->tokenState = TOKEN_CLOSED;
    pthread_cond_broadcast(&pThreadPool->tokenCond);
    pthread_mutex_unlock(&pThreadPool->taskQueue.mutex);
    return -1;
}

// 只有主线程调用，控制连接的请求和回复保持串行
int refresh_token(int sockFd, ThreadPool *pThreadPool)
{
    char buf[64];
    ssize_t ret;
    do
    {
        ret = read(pThreadPool->tokenPipe[0], buf, sizeof(buf));
    }while(ret > 0 || (ret == -1 && errno == EINTR));
    if(ret == 0 || (ret == -1 && errno != EAGAIN))
    {
        close_token(pThreadPool);
        return -1;
    }
    pthread_mutex_lock(&pThreadPool->taskQueue.mutex);
    if(pThreadPool->tokenState != TOKEN_REQUESTED)
    {
        int closed = pThreadPool->tokenState == TOKEN_CLOSED;
        pthread_mutex_unlock(&pThreadPool->taskQueue.mutex);
        return closed ? -1 : 0;
    }
    pThreadPool->tokenState = TOKEN_REFRESHING;
    pthread_mutex_unlock(&pThreadPool->taskQueue.mutex);

    train_state_t train;
    bzero(&train, sizeof(train));
    train.state = REFRESH;
    train.length = 7;
    train.pre_length = 7;
    memcpy(train.buf, "refresh", train.length);
    int length = sizeof(train.length) + sizeof(train.state) +
                 sizeof(train.pre_length) + train.length;
    int reply;
    char token[PROTOCOL_TOKEN_CAPACITY];
    int64_t deadline;
    if(wire_send_command(sockFd, &train, MSG_NOSIGNAL) != length ||
       wire_recv_int(sockFd, &reply) != sizeof(reply) ||
       reply != TOKEN_REFRESH_REPLY || receive_token(sockFd, token, &deadline) != 0)
    {
        close_token(pThreadPool);
        return -1;
    }
    pthread_mutex_lock(&pThreadPool->taskQueue.mutex);
    if(pThreadPool->tokenState != TOKEN_REFRESHING)
    {
        pthread_mutex_unlock(&pThreadPool->taskQueue.mutex);
        return -1;
    }
    strcpy(pThreadPool->token, token);
    pThreadPool->tokenDeadline = deadline;
    ++pThreadPool->tokenVersion;
    pThreadPool->tokenState = TOKEN_READY;
    pthread_cond_broadcast(&pThreadPool->tokenCond);
    pthread_mutex_unlock(&pThreadPool->taskQueue.mutex);
    return 0;
}
