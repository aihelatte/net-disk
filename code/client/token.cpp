#include "threadPool.h"
#include <chrono>

// 按长度、Token 正文和生命周期的顺序读取。成功记录本地截止时间，失败返回 -1。
int receive_token(int sockFd, char *token, int64_t *pDeadline)
{
    int length;
    int lifetime;
    int64_t now = network_time_ms();
    if (now < 0)
    {
        return -1;
    }
    if (wire_recv_int(sockFd, &length) != sizeof(length))
    {
        return -1;
    }
    if (length <= 0 || !protocol_valid_text_length(length, PROTOCOL_TOKEN_CAPACITY))
    {
        return -1;
    }
    if (recvn(sockFd, token, length) != length)
    {
        return -1;
    }
    if (memchr(token, '\0', length) != NULL)
    {
        return -1;
    }
    if (wire_recv_int(sockFd, &lifetime) != sizeof(lifetime))
    {
        return -1;
    }
    if (lifetime != TOKEN_LIFETIME)
    {
        return -1;
    }
    token[length] = '\0';
    *pDeadline = now + (int64_t)lifetime * 1000;// 仅用于提前续签，服务端负责最终有效期校验
    return 0;
}

// 持锁标记凭证关闭，并唤醒所有正在等待续签的工作线程。
void close_token(ThreadPool *pThreadPool)
{
    std::lock_guard<std::mutex> lock(pThreadPool->taskQueue.mutex);
    pThreadPool->tokenState = TokenState::closed;
    pThreadPool->tokenCond.notify_all();
}

// 获取当前可用 Token。过期或强制续签时通知主线程，成功返回 0，失败返回 -1。
int get_token(ThreadPool *pThreadPool, char *token,
              unsigned long *pVersion, int forceRefresh)
{
    if (network_time_ms() < 0)
    {
        return -1;
    }
    // 使用单调时钟记录一次总截止时间，重复唤醒也不会重新开始计时。
    const std::chrono::steady_clock::time_point timeout =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(NETWORK_IDLE_TIMEOUT_MS);
    std::unique_lock<std::mutex> lock(pThreadPool->taskQueue.mutex);
    while (pThreadPool->tokenState != TokenState::closed)
    {
        if (forceRefresh && *pVersion != pThreadPool->tokenVersion)
        {
            forceRefresh = 0;// 每次唤醒后重查，避免同一代凭证重复续签
        }
        int64_t now = network_time_ms();
        if (now < 0)
        {
            break;
        }
        if (pThreadPool->tokenState == TokenState::ready && !forceRefresh &&
           now + TOKEN_REFRESH_MARGIN_MS < pThreadPool->tokenDeadline)
        {
            strcpy(token, pThreadPool->token);
            *pVersion = pThreadPool->tokenVersion;
            return 0;
        }
        if (pThreadPool->tokenState == TokenState::ready)
        {
            pThreadPool->tokenState = TokenState::requested;
            // 已在锁内标记续签请求。写通知管道时让出锁，以便主线程读取请求状态。
            lock.unlock();
            ssize_t ret;
            do
            {
                ret = write(pThreadPool->tokenPipe[1].get(), "1", 1);
            } while (ret == -1 && errno == EINTR);
            int notifyFailed = ret != 1 && !(ret == -1 && errno == EAGAIN);
            // 后续要读取或更新共享状态，先重新取得同一把锁。
            lock.lock();
            if (notifyFailed)
            {
                break;
            }
            forceRefresh = 0;
            continue;
        }
        // 等待会临时释放队列锁，收到通知后重新加锁并检查 Token 状态。
        if (pThreadPool->tokenCond.wait_until(lock, timeout) == std::cv_status::timeout)
        {
            break;
        }
    }
    pThreadPool->tokenState = TokenState::closed;
    pThreadPool->tokenCond.notify_all();
    return -1;
}

// 只有主线程调用，控制连接的请求和回复保持串行
int refresh_token(int sockFd, ThreadPool *pThreadPool)
{
    char buf[64];
    ssize_t ret;
    do
    {
        ret = read(pThreadPool->tokenPipe[0].get(), buf, sizeof(buf));
    } while (ret > 0 || (ret == -1 && errno == EINTR));
    if (ret == 0 || (ret == -1 && errno != EAGAIN))
    {
        close_token(pThreadPool);
        return -1;
    }
    {
        std::lock_guard<std::mutex> lock(pThreadPool->taskQueue.mutex);
        if (pThreadPool->tokenState == TokenState::closed)
        {
            return -1;
        }
        if (pThreadPool->tokenState != TokenState::requested)
        {
            return 0;
        }
        pThreadPool->tokenState = TokenState::refreshing;
    }

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
    if (wire_send_command(sockFd, &train, MSG_NOSIGNAL) != length ||
       wire_recv_int(sockFd, &reply) != sizeof(reply) ||
       reply != TOKEN_REFRESH_REPLY || receive_token(sockFd, token, &deadline) != 0)
    {
        close_token(pThreadPool);
        return -1;
    }
    std::lock_guard<std::mutex> lock(pThreadPool->taskQueue.mutex);
    if (pThreadPool->tokenState != TokenState::refreshing)
    {
        return -1;
    }
    strcpy(pThreadPool->token, token);
    pThreadPool->tokenDeadline = deadline;
    ++pThreadPool->tokenVersion;
    pThreadPool->tokenState = TokenState::ready;
    pThreadPool->tokenCond.notify_all();
    return 0;
}
