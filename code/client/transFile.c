#include "threadPool.h"
#include "worker.h"

// 自己实现MSG_WAITALL
int recvn(int sockFd, void *pstart, int len)
{
    return network_recvn(sockFd, pstart, len);
}

int sendn(int sockFd, const void *pstart, int len, int flags)
{
    return network_sendn(sockFd, pstart, len, flags);
}

int send_keepalive(int sockFd)
{
    train_state_t train;
    bzero(&train, sizeof(train));
    train.state = KEEPALIVE;
    train.length = 4;
    train.pre_length = 4;
    memcpy(train.buf, "ping", train.length);
    int length = sizeof(train.length) + sizeof(train.state) +
                 sizeof(train.pre_length) + train.length;
    return wire_send_command(sockFd, &train, MSG_NOSIGNAL) == length ? 0 : -1;
}

int writen(int fd, const void *pstart, int len)
{
    int total = 0;
    const char *p = (const char*)pstart;
    if(len < 0)
    {
        return -1;
    }
    while(total < len)
    {
        int ret = write(fd, p + total, len - total);
        if(ret > 0)
        {
            total += ret;
            continue;
        }
        if(ret == -1 && errno == EINTR)
        {
            continue;
        }
        return ret;
    }
    return total;
}

//给服务器上传内容
int Upload(int netFd, char *file, int fd, const char *md5_str)
{
    train_t train;

    // 相对路径只用于打开本地文件，传输时发送最后一段文件名
    char *name = strrchr(file, '/');
    if(name == NULL)
    {
        name = file;
    }
    else
    {
        ++name;
    }
    if(!is_valid_file_name(name, strlen(name)))
    {
        return EXIT_FAILURE;
    }
    train.length = strlen(name);
    strcpy(train.buf, name);
    int ret = wire_send_train(netFd, &train, MSG_NOSIGNAL);
    if(ret != (int)sizeof(train.length) + train.length)
    {
        return EXIT_FAILURE;
    }

    // 发送文件总大小
    struct stat statbuf;
    ret = fstat(fd, &statbuf);
    if(ret == -1)
    {
        perror("fstat");
        return EXIT_FAILURE;
    }
    train.length = sizeof(int64_t);
    int64_t fileSize = statbuf.st_size;// 获得文件总大小
    wire_put_i64(train.buf, fileSize);
    if(wire_send_train(netFd, &train, MSG_NOSIGNAL) != (int)sizeof(train.length) + train.length)
    {
        return EXIT_FAILURE;
    }

    // MD5已在数据连接建立前计算，直接发送
    train.length = strlen(md5_str);
    strcpy(train.buf, md5_str);
    if(wire_send_train(netFd, &train, MSG_NOSIGNAL) != (int)sizeof(train.length) + train.length)
    {
        return EXIT_FAILURE;
    }

    // 确认是否需要上传
    int check;
    if(wire_recv_int(netFd, &check) != sizeof(check))
    {
        return EXIT_FAILURE;
    }
    if(check == 0)// 不需要上传
    {
        int total_time;
        if(wire_recv_int(netFd, &total_time) != sizeof(total_time) || total_time < 0)
        {
            return EXIT_FAILURE;
        }
        printf("\033[1;37;46m本次上传耗时%ds^_^\033[0m\n", total_time);
        return 0;
    }

    if(check != 1)
    {
        return EXIT_FAILURE;
    }

    // 获得文件已传大小
    int64_t exitSize = 0;
    if(wire_recv_i64(netFd, &exitSize) != sizeof(exitSize))
    {
        return EXIT_FAILURE;
    }
    if(exitSize < 0 || exitSize > fileSize)
    {
        return EXIT_FAILURE;
    }

    // printf("time to puts!\n");

    // 发送文件内容
    // 判断文件大小，大文件使用sendfile分块传输
    // printf("exitSize = %lld\n", (long long)exitSize);
    // printf("fileSize = %lld\n", (long long)fileSize);
    if(fileSize >= 1073741824)// 大于1G，使用sendfile发送
    {
        off_t offset = exitSize;
        while(offset < fileSize)
        {
            int sendLength;
            if(fileSize - offset < LARGE_TRANSFER_SIZE)
            {
                sendLength = fileSize - offset;
            }
            else
            {
                sendLength = LARGE_TRANSFER_SIZE;
            }
            ssize_t sendRet = network_sendfile(netFd, fd, &offset, sendLength);
            if(sendRet > 0)
            {
                continue;
            }
            if(sendRet == -1 && errno == EINTR)
            {
                continue;
            }
            return EXIT_FAILURE;
        }
    }
    else
    {
        char *p = NULL;
        if(fileSize > 0)
        {
            p = (char*)mmap(NULL, fileSize, PROT_READ, MAP_SHARED, fd, 0);
            if(p == MAP_FAILED)
            {
                perror("mmap");
                return EXIT_FAILURE;
            }
        }
        int64_t total = exitSize;
        // 小于1G用小火车分次传输
        while (total < fileSize)
        {
            if (fileSize - total < (int64_t)sizeof(train.buf))   //最后一次传输剩余部分
            {
                train.length = fileSize - total;
            }
            else        //直接传满
            {
                train.length = sizeof(train.buf);
            }
            memcpy(train.buf, p + total, train.length);
            total += train.length;
            if(wire_send_train(netFd, &train, MSG_NOSIGNAL) != (int)sizeof(train.length) + train.length)
            {
                munmap(p, fileSize);
                return EXIT_FAILURE;
            }
        }
        // 发送长度为0代表发送结束
        train.length = 0;
        ret = wire_send_train(netFd, &train, MSG_NOSIGNAL);
        if(ret != (int)sizeof(train.length) + train.length)
        {
            if(fileSize > 0)
            {
                munmap(p, fileSize);
            }
            return EXIT_FAILURE;
        }
        if(fileSize > 0)
        {
            munmap(p, fileSize);
        }
    }

    int total_time;
    do
    {
        if(wire_recv_int(netFd, &total_time) != sizeof(total_time))
        {
            return EXIT_FAILURE;
        }
    } while(total_time == TRANSFER_VERIFY_PROGRESS);
    if(total_time < 0)
    {
        if(total_time == TRANSFER_VERIFY_FAILED)
        {
            fprintf(stderr, "Upload verification failed. If the file changed, remove the incomplete same-name entry before retrying.\n");
        }
        return EXIT_FAILURE;
    }
    printf("\033[1;37;46m本次上传耗时%ds^_^\033[0m\n",total_time);
    return 0;
}

int Download(int sockFd)
{
    // 先获取文件名
    char name[1024] = {0};
    int dataLength;
    if(wire_recv_int(sockFd, &dataLength) != sizeof(int))
    {
        return EXIT_FAILURE;
    }
    if(dataLength == 0)
    {
        printf("\033[1;31mWrong file name!\033[0m\n");
        return EXIT_FAILURE;
    }
    if(dataLength < 0 || dataLength >= (int)sizeof(name) ||
       recvn(sockFd, name, dataLength) != dataLength ||
       !is_valid_file_name(name, dataLength))
    {
        return EXIT_FAILURE;
    }
    // printf("name = %s\n", name);
    
    int fd = open_in_root("download", name, O_RDWR | O_CREAT, 0666);// 文件不存在就创建
    ERROR_CHECK(fd, -1, "open");
    if(flock(fd, LOCK_EX | LOCK_NB) == -1)
    {
        close(fd);
        return EXIT_FAILURE;
    }
    char buf[1000] = {0};

    // 获取文件大小
    int64_t fileSize;
    if(wire_recv_int(sockFd, &dataLength) != sizeof(int) ||
       dataLength != (int)sizeof(fileSize) ||
       wire_recv_i64(sockFd, &fileSize) != dataLength)
    {
        close(fd);
        return EXIT_FAILURE;
    }
    if(fileSize < 0)
    {
        close(fd);
        return EXIT_FAILURE;
    }

    // 断点重传:
    // 检查本地是否有文件存在，不考虑有不同文件但文件名相同的情况，存在则文件大小不为0
    struct stat statbuf;
    int ret = fstat(fd, &statbuf);
    if(ret == -1)
    {
        perror("fstat");
        close(fd);
        return EXIT_FAILURE;
    }
    int64_t exitSize = statbuf.st_size;
    if(exitSize < 0 || exitSize > fileSize)
    {
        close(fd);
        return EXIT_FAILURE;
    }
    if(wire_send_i64(sockFd, exitSize, MSG_NOSIGNAL) != sizeof(exitSize))
    {
        close(fd);
        return EXIT_FAILURE;
    }
    if(lseek(fd, exitSize, SEEK_SET) == -1)
    {
        close(fd);
        return EXIT_FAILURE;
    }

    time_t timeBeg, timeEnd;

    // 获取文件内容
    // 判断文件大小，是否使用映射内存接收
    timeBeg = time(NULL);
    if(fileSize >= 1073741824)// 大于1G，直接接收到mmap映射内存
    {
        if((int64_t)(off_t)fileSize != fileSize || (uint64_t)fileSize > SIZE_MAX ||
           ftruncate(fd, fileSize) == -1)
        {
            close(fd);
            return EXIT_FAILURE;
        }
        char *p = (char *)mmap(NULL, fileSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if(p == MAP_FAILED)
        {
            perror("mmap");
            if(ftruncate(fd, exitSize) == -1)
            {
                perror("ftruncate");
            }
            close(fd);
            return EXIT_FAILURE;
        }
        // total只记录已经完整接收的连续块，失败时当前块由下次重传
        int64_t total = exitSize;
        while(total < fileSize)
        {
            int recvLength;
            if(fileSize - total < LARGE_TRANSFER_SIZE)
            {
                recvLength = fileSize - total;
            }
            else
            {
                recvLength = LARGE_TRANSFER_SIZE;
            }
            if(recvn(sockFd, p + total, recvLength) != recvLength)
            {
                munmap(p, fileSize);
                if(ftruncate(fd, total) == -1)
                {
                    perror("ftruncate");
                }
                close(fd);
                return EXIT_FAILURE;
            }
            total += recvLength;
        }
        munmap(p, fileSize);
        timeEnd = time(NULL);
    }
    else // 小文件分次传
    {
        int64_t total = exitSize;
        while (1)
        {
            bzero(buf, sizeof(buf));
            ret = wire_recv_int(sockFd, &dataLength);
            if (ret != sizeof(int))// 对面连接断开或长度没有接收完整
            {
                close(fd);
                return EXIT_FAILURE;
            }
            if (dataLength == 0)
            {
                if(total != fileSize)
                {
                    close(fd);
                    return EXIT_FAILURE;
                }
                timeEnd = time(NULL);
                break;
            }
            if(dataLength < 0 || dataLength > (int)sizeof(buf) ||
               dataLength > fileSize - total || recvn(sockFd, buf, dataLength) != dataLength)
            {
                close(fd);
                return EXIT_FAILURE;
            }
            ret = writen(fd, buf, dataLength);
            if(ret != dataLength)
            {
                close(fd);
                return EXIT_FAILURE;
            }
            total += dataLength;
        }
    }
    close(fd);
    int total_time = timeEnd - timeBeg;
    printf("\033[1;37;46m本次下载耗时%ds^_^\033[0m\n",total_time);
    return wire_send_int(sockFd, total_time, MSG_NOSIGNAL) == sizeof(int) ? 0 : EXIT_FAILURE;
}
