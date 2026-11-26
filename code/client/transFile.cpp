#include "file_descriptor.h"
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
        perror("\nfstat");
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
        printf("\n\033[1;37;46m本次上传耗时%ds^_^\033[0m\n", total_time);
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


    // 发送文件内容
    // 判断文件大小，大文件使用sendfile分块传输
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
                perror("\nmmap");
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
            fprintf(stderr, "\nUpload verification failed. If the file changed, remove the incomplete same-name entry before retrying.\n");
        }
        return EXIT_FAILURE;
    }
    printf("\n\033[1;37;46m本次上传耗时%ds^_^\033[0m\n",total_time);
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
        printf("\n\033[1;31mWrong file name!\033[0m\n");
        return EXIT_FAILURE;
    }
    if(dataLength < 0 || dataLength >= (int)sizeof(name) ||
       recvn(sockFd, name, dataLength) != dataLength ||
       !is_valid_file_name(name, dataLength))
    {
        return EXIT_FAILURE;
    }
    // 获取文件大小
    int64_t fileSize;
    if(wire_recv_int(sockFd, &dataLength) != sizeof(int) ||
       dataLength != (int)sizeof(fileSize) ||
       wire_recv_i64(sockFd, &fileSize) != dataLength)
    {
        return EXIT_FAILURE;
    }
    if(fileSize < 0)
    {
        return EXIT_FAILURE;
    }

    char expectedMd5[MD5_STR_LEN + 1] = {};
    if(wire_recv_int(sockFd, &dataLength) != sizeof(int) || dataLength != MD5_STR_LEN ||
       recvn(sockFd, expectedMd5, dataLength) != dataLength ||
       strspn(expectedMd5, "0123456789abcdefABCDEF") != MD5_STR_LEN)
    {
        return EXIT_FAILURE;
    }

    FileDescriptor downloads(open("download", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if(downloads.get() == -1)
    {
        perror("\nopen download");
        return EXIT_FAILURE;
    }
    struct stat destination;
    if(fstatat(downloads.get(), name, &destination, AT_SYMLINK_NOFOLLOW) == 0)
    {
        fprintf(stderr, "\nDownload conflict: %s already exists. Rename it to download again.\n", name);
        return EXIT_FAILURE;
    }
    if(errno != ENOENT)
    {
        perror("\nstat destination");
        return EXIT_FAILURE;
    }

    const char *partialDir = ".netdisk-partial";
    if(mkdirat(downloads.get(), partialDir, 0700) == -1 && errno != EEXIST)
    {
        perror("\nmkdir partial");
        return EXIT_FAILURE;
    }
    FileDescriptor partial(openat(downloads.get(), partialDir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if(partial.get() == -1)
    {
        perror("\nopen partial directory");
        return EXIT_FAILURE;
    }
    FileDescriptor localFile(openat(partial.get(), expectedMd5,
                                    O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600));
    int fd = localFile.get();
    struct stat info, entry;
    if(fd == -1 || flock(fd, LOCK_EX | LOCK_NB) == -1 || fstat(fd, &info) == -1 ||
       !S_ISREG(info.st_mode) || fstatat(partial.get(), expectedMd5, &entry, AT_SYMLINK_NOFOLLOW) == -1 ||
       entry.st_ino != info.st_ino || entry.st_dev != info.st_dev)
    {
        fprintf(stderr, "\nCannot lock partial download for %s.\n", name);
        return EXIT_FAILURE;
    }
    // 上次可能在发布后、删除临时名之前退出。已发布的文件不能再作为可写断点。
    if(info.st_nlink > 1)
    {
        if(unlinkat(partial.get(), expectedMd5, 0) == -1)
        {
            perror("\nremove stale partial link");
        }
        else
        {
            fprintf(stderr, "\nRecovered a published download. Retry gets %s.\n", name);
        }
        return EXIT_FAILURE;
    }
    // 临时文件只追加已收到的字节，进程退出后长度仍是实际续传位置。
    int64_t total = info.st_size;
    if(total > fileSize)
    {
        if(ftruncate(fd, 0) == -1)
        {
            return EXIT_FAILURE;
        }
        total = 0;
    }
    if(lseek(fd, total, SEEK_SET) == -1 ||
       wire_send_i64(sockFd, total, MSG_NOSIGNAL) != sizeof(total))
    {
        return EXIT_FAILURE;
    }

    time_t timeBeg = time(NULL);
    char buf[64 * 1024];
    const bool rawStream = fileSize >= 1073741824;
    while(true)
    {
        if(rawStream)
        {
            if(total == fileSize)
            {
                break;
            }
            dataLength = fileSize - total < (int64_t)sizeof(buf) ? (int)(fileSize - total) : (int)sizeof(buf);
        }
        else
        {
            if(wire_recv_int(sockFd, &dataLength) != sizeof(int))
            {
                return EXIT_FAILURE;
            }
            if(dataLength == 0 && total == fileSize)
            {
                break;
            }
        }
        if(dataLength <= 0 || dataLength > (int)sizeof(buf) || dataLength > fileSize - total ||
           recvn(sockFd, buf, dataLength) != dataLength || writen(fd, buf, dataLength) != dataLength)
        {
            return EXIT_FAILURE;
        }
        total += dataLength;
    }

    char actualMd5[MD5_STR_LEN + 1];
    if(Compute_fd_md5(fd, actualMd5, sockFd) != 0)
    {
        return EXIT_FAILURE;
    }
    if(strcasecmp(actualMd5, expectedMd5) != 0)
    {
        fprintf(stderr, "\nDownload checksum mismatch for %s. Retry to download again.\n", name);
        if(ftruncate(fd, 0) == -1)
        {
            perror("\nreset partial download");
        }
        return EXIT_FAILURE;
    }
    // linkat 原子创建正式文件名，其他任务先占用该名称时会返回 EEXIST。
    if(linkat(partial.get(), expectedMd5, downloads.get(), name, 0) == -1)
    {
        if(errno == EEXIST)
        {
            fprintf(stderr, "\nDownload conflict: %s already exists. Rename it to download again.\n", name);
        }
        else
        {
            perror("\npublish download");
        }
        return EXIT_FAILURE;
    }
    if(unlinkat(partial.get(), expectedMd5, 0) == -1)
    {
        perror("\ndownload published, cannot remove partial link");
    }
    int total_time = time(NULL) - timeBeg;
    printf("\n本次下载耗时%ds\n", total_time);
    return wire_send_int(sockFd, total_time, MSG_NOSIGNAL) == sizeof(int) ? 0 : EXIT_FAILURE;
}
