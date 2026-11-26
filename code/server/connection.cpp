#include "protocol_validate.h"
#include "worker.h"

// 把回复追加到缓冲区，由主线程在 epoll 中继续发送。
// 成功返回入队字节数，超过缓冲上限或分配失败时返回 -1。
int Connection::queueReply(const void *buf, int length)
{
    if (length < 0 || (size_t)length > MAX_CONTROL_REPLY - sendBuf.size())
    {
        errno = EMSGSIZE;
        return -1;
    }
    if (length == 0)
    {
        return 0;
    }
    const char *bytes = static_cast<const char *>(buf);
    try
    {
        sendBuf.insert(sendBuf.end(), bytes, bytes + length);
    } catch(const std::bad_alloc &) {
        return -1;
    }
    return length;
}

// 从已发送位置继续发送。返回本次发送字节数，遇到不可恢复的错误返回 -1。
int Connection::flushReply()
{
    int total = 0;
    // 单次事件最多发送64 KiB，给其他连接和时间轮保留处理机会
    while (sendOffset < sendBuf.size() && total < 64 * 1024)
    {
        size_t length = sendBuf.size() - sendOffset;
        if (length > (size_t)(64 * 1024 - total))
        {
            length = 64 * 1024 - total;
        }
        ssize_t ret = send(fd(),
                           sendBuf.data() + sendOffset,
                           length, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (ret > 0)
        {
            sendOffset += ret;
            total += ret;
        }
        else if (ret == -1 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        {
            return total;
        }
        else
        {
            return -1;
        }
    }
    if (sendOffset == sendBuf.size())
    {
        std::vector<char>().swap(sendBuf);
        sendOffset = 0;
    }
    return total;
}

bool Connection::validHeader() const
{
    if (!protocol_valid_text_length(train.length, sizeof(train.buf)) ||
       train.state < REGISTER || train.state > REFRESH ||
       !protocol_valid_pre_length(train.length, train.pre_length))
    {
        return false;
    }

    switch (train.state)
    {
    case REGISTER:
    case LOGIN:
        return protocol_valid_text_length(train.length,
                                          PROTOCOL_USER_NAME_CAPACITY) &&
               train.length > 0 && train.pre_length == 0;
    case LS:
        return protocol_valid_text_length(train.length,
                                          PROTOCOL_COMMAND_CAPACITY) &&
               train.length >= 2 && train.pre_length == 2;
    case CD:
    case REMOVE:
        return protocol_valid_text_length(train.length,
                                          PROTOCOL_COMMAND_CAPACITY) &&
               train.length > 3 && train.pre_length == 2;
    case PWD:
        return protocol_valid_text_length(train.length,
                                          PROTOCOL_COMMAND_CAPACITY) &&
               train.length == 3 && train.pre_length == 3;
    case MKDIR:
        return protocol_valid_text_length(train.length,
                                          PROTOCOL_COMMAND_CAPACITY) &&
               train.length > 6 && train.pre_length == 5;
    case GETS:
    case PUTS:
        return protocol_valid_text_length(train.length,
                                          PROTOCOL_COMMAND_CAPACITY) &&
               train.length > 5 && train.pre_length == 4;
    case TOKEN:
        return protocol_valid_split_text(train.length,
                                         train.pre_length,
                                         PROTOCOL_USER_NAME_CAPACITY,
                                         PROTOCOL_TOKEN_CAPACITY);
    case KEEPALIVE:
        return train.length == 4 && train.pre_length == 4;
    case REFRESH:
        return train.length == 7 && train.pre_length == 7;
    default:
        return false;
    }
}

bool Connection::validBody() const
{
    switch (train.state)
    {
    case KEEPALIVE:
        return strcmp(train.buf, "ping") == 0;
    case REFRESH:
        return strcmp(train.buf, "refresh") == 0;
    case REGISTER:
    case LOGIN:
    case TOKEN:
        return true;
    case LS:
        return strncmp(train.buf, "ls", 2) == 0 &&
               (train.length == 2 || train.buf[2] == ' ');
    case CD:
        return strncmp(train.buf, "cd ", 3) == 0;
    case PWD:
        return strcmp(train.buf, "pwd") == 0;
    case REMOVE:
        return strncmp(train.buf, "rm ", 3) == 0;
    case MKDIR:
        return strncmp(train.buf, "mkdir ", 6) == 0;
    case GETS:
        return strncmp(train.buf, "gets ", 5) == 0;
    case PUTS:
        return strncmp(train.buf, "puts ", 5) == 0;
    default:
        return false;
    }
}

// 上一帧已处理，清空接收进度并从下一帧的长度字段开始读取。
void Connection::resetRequest()
{
    train = {};
    received = 0;
    needed = sizeof(int);
}

// 只读取当前帧所需字节，交接后的数据仍留给工作线程。
// 返回 1 表示收到完整帧，0 表示继续等待，-1 表示连接断开或帧无效。
int Connection::receiveRequest(bool &progress)
{
    progress = false;
    while (1)
    {
        ssize_t ret = recv(fd(),
                           (char *)&train + received,
                           needed - received,
                           MSG_DONTWAIT);
        if (ret > 0)
        {
            progress = true;
            received += ret;
            if (received < needed)
            {
                continue;
            }
            if (received == sizeof(int))
            {
                int length = wire_get_i32(&train.length);
                train.length = length;
                if (stage == RECEIVE_COMMAND)
                {
                    if (length <= 0 || !protocol_valid_text_length(length, sizeof(train.buf)))
                    {
                        return -1;
                    }
                    needed = 3 * sizeof(int);
                }
                else
                {
                    if (length != 49)
                    {
                        return -1;
                    }
                    needed = sizeof(int) + length;
                }
                continue;
            }
            if (stage == RECEIVE_COMMAND &&
               received == 3 * sizeof(int))
            {
                train.state = wire_get_i32(&train.state);
                train.pre_length = wire_get_i32(&train.pre_length);
                if (!validHeader())
                {
                    return -1;
                }
                needed = 3 * sizeof(int) + train.length;
                continue;
            }
            if (stage == RECEIVE_COMMAND)
            {
                train.buf[train.length] = '\0';
                if (memchr(train.buf, '\0', train.length) != NULL ||
                   !validBody())
                {
                    return -1;
                }
            }
            else if (memchr((char *)&train + sizeof(int), '\0', 49) != NULL)
            {
                return -1;
            }
            return 1;
        }
        if (ret == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        {
            return 0;// 半包保留在连接中，回到epoll处理其他连接
        }
        return -1;
    }
}
