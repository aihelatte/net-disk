#include "threadPool.h"
#include "worker.h"

// 自己实现MSG_WAITALL
int recvn(int sockFd, void *pstart, int len)
{
    int total = 0;
    char *p = (char*)pstart;
    if(len < 0)
    {
        return -1;
    }
    while(total < len)// 收完为止
    {
        int ret = recv(sockFd, p + total, len - total, 0);
        if(ret > 0)
        {
            total += ret;
            continue;
        }
        if(ret == 0)// 对端断开，返回实际收到的字节数
        {
            return total;
        }
        if(errno == EINTR)
        {
            continue;
        }
        perror("recv");
        return -1;
    }
    return total;
}

//给服务器上传内容
int Upload(int netFd, char *file)
{
    char file_path[1024] = {0};
    sprintf(file_path, "%s%s%s","upload", "/", file);
    int fd = open(file_path, O_RDWR);
    train_t train;
    if(fd == -1)// 打开文件错误需要通知对面
    {
        perror("open");
        bzero(&train, sizeof(train));
        train.length = 0;
        send(netFd, &train, sizeof(train.length) + train.length, 0);
    }

    // 发送文件名
    train.length = strlen(file);
    strcpy(train.buf, file);
    int ret = send(netFd, &train, sizeof(train.length) + train.length, MSG_NOSIGNAL);
    ERROR_CHECK(ret, -1, "send");

    // 发送文件总大小
    struct stat statbuf;
    ret = fstat(fd, &statbuf);
    ERROR_CHECK(ret, -1, "fstat");
    train.length = 4;
    int fileSize = statbuf.st_size;// 获得文件总大小
    memcpy(train.buf, &fileSize, sizeof(int));
    send(netFd, &train, sizeof(train.length) + train.length, MSG_NOSIGNAL);

    // 计算并发送文件md5值(这里大文件可能耗时会比较久)
    char md5_str[MD5_STR_LEN + 1] = {0};
    Compute_file_md5(file_path, md5_str);
    train.length = strlen(md5_str);
    strcpy(train.buf, md5_str);
    send(netFd, &train, sizeof(train.length) + train.length, MSG_NOSIGNAL);

    // 确认是否需要上传
    int check;
    if(recvn(netFd, &check, sizeof(check)) != sizeof(check))
    {
        close(fd);
        return EXIT_FAILURE;
    }
    if(check == 0)// 不需要上传
    {
        printf("\033[1;37;46m本次上传耗时0s^_^\033[0m\n");
        return 0;
    }

    // 获得文件已传大小
    int exitSize = 0;
    if(recvn(netFd, &exitSize, sizeof(exitSize)) != sizeof(exitSize))
    {
        close(fd);
        return EXIT_FAILURE;
    }

    // printf("time to puts!\n");

    // 发送文件内容
    // 判断文件大小，大文件用一次性传输
    char *p = (char*)mmap(NULL, fileSize, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    int total = exitSize;
    // printf("exitSize = %d\n", exitSize);
    // printf("fileSize = %d\n", fileSize);
    if(fileSize >= 1073741824)// 大于1G，通过mmap实现零拷贝
    {
        send(netFd, p + total, fileSize, MSG_NOSIGNAL);
        printf("send once!\n");
    }
    else
    {
        // 小于1G用小火车分次传输
        while (total < fileSize)
        {
            if (fileSize - total < sizeof(train.buf))   //最后一次传输剩余部分
            {
                train.length = fileSize - total;
            }
            else        //直接传满
            {
                train.length = sizeof(train.buf);
            }
            memcpy(train.buf, p + total, train.length);
            total += train.length;
            send(netFd, &train, sizeof(train.length) + train.length, MSG_NOSIGNAL);
        }
        // 发送长度为0代表发送结束
        train.length = 0;
        ret = send(netFd, &train, sizeof(train.length) + train.length, MSG_NOSIGNAL);
        ERROR_CHECK(ret, -1, "send");
    }

    close(fd);
    munmap(p, fileSize);
    int total_time;
    if(recvn(netFd, &total_time, sizeof(int)) != sizeof(int))
    {
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
    if(recvn(sockFd, &dataLength, sizeof(int)) != sizeof(int))
    {
        return EXIT_FAILURE;
    }
    if(dataLength == 0)
    {
        printf("\033[1;31mWrong file name!\033[0m\n");
        return EXIT_FAILURE;
    }
    if(dataLength < 0 || recvn(sockFd, name, dataLength) != dataLength)
    {
        return EXIT_FAILURE;
    }
    // printf("name = %s\n", name);
    
    char file_path[1024] = {0};
    sprintf(file_path, "%s%s%s","download", "/", name);
    int fd = open(file_path, O_RDWR | O_CREAT, 0666);// 文件不存在就创建
    ERROR_CHECK(fd, -1, "open");
    char buf[1000] = {0};

    // 获取文件大小
    int fileSize;
    if(recvn(sockFd, &dataLength, sizeof(int)) != sizeof(int) ||
       dataLength < 0 ||
       recvn(sockFd, &fileSize, dataLength) != dataLength)
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
    ERROR_CHECK(ret, -1, "fstat");
    int exitSize = statbuf.st_size;
    send(sockFd, &exitSize, sizeof(exitSize), MSG_NOSIGNAL);
    lseek(fd, 0, SEEK_END);

    time_t timeBeg, timeEnd;

    // 获取文件内容
    // 判断文件大小，是否一次接收
    timeBeg = time(NULL);
    if(fileSize >= 1073741824)// 大于1G
    {
        ftruncate(fd, fileSize); // 预设文件空洞(直接忽略原本文件已经存在的情况)
        char *p = (char *)mmap(NULL, fileSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ERROR_CHECK(p, MAP_FAILED, "mmap");
        if(recvn(sockFd, p, fileSize) != fileSize)
        {
            munmap(p, fileSize);
            close(fd);
            return EXIT_FAILURE;
        }
        munmap(p, fileSize);
        timeEnd = time(NULL);
    }
    else // 小文件分次传
    {
        while (1)
        {
            bzero(buf, sizeof(buf));
            ret = recv(sockFd, &dataLength, sizeof(int), 0);
            if (dataLength == 0)
            {
                timeEnd = time(NULL);
                break;
            }
            if (ret == 0)// 对面连接断开
            {
                close(fd);
                return EXIT_FAILURE;
            }
            if(dataLength < 0 || recvn(sockFd, buf, dataLength) != dataLength)
            {
                close(fd);
                return EXIT_FAILURE;
            }
            write(fd, buf, dataLength);
        }
    }
    close(fd);
    int total_time = timeEnd - timeBeg;
    printf("\033[1;37;46m本次下载耗时%ds^_^\033[0m\n",total_time);
    send(sockFd, &total_time, sizeof(int), MSG_NOSIGNAL);
}
