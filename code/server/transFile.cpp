#include "file_descriptor.h"
#include"threadPool.h"
#include"worker.h"
// TCP是一种字节流传输，信息无边界，对方接收时以最大字节数为界限
// 利用TLV协议，先传输文件大小，固定的int64_t类型，8个字节，再传输文件内容
int recvn(int sockFd, void *pstart, int len)
{
    return network_recvn(sockFd, pstart, len);
}

int sendn(int sockFd, const void *pstart, int len, int flags)
{
    return network_sendn(sockFd, pstart, len, flags);
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

int Upload(int netFd, char *file, int user_id, int code)
{
    // 打开文件之前需要先获取文件在本地的真实名称
    char md5_str[MD5_STR_LEN + 1] = {0};
    
    //找到对应文件的md5值
    if(get_virtual_file_md5(file, user_id, code, md5_str) != 0)
    {
        int length = 0;
        wire_send_int(netFd, length, MSG_NOSIGNAL);
        return EXIT_FAILURE;
    }
    char real_file_name[1024] = {0};

    if(get_real_file_name(md5_str, real_file_name) != 0)
    {
        int length = 0;
        wire_send_int(netFd, length, MSG_NOSIGNAL);
        return EXIT_FAILURE;
    }
    int fd = open_in_root("NetDisk", real_file_name, O_RDONLY, 0);// 打开本地文件
    FileDescriptor localFile(fd);
    train_t train;
    char *name = strrchr(file, '/');
    if(name == NULL)
    {
        name = file;
    }
    else
    {
        ++name;
    }
    if(fd == -1 || !is_valid_file_name(name, strlen(name)))// 打开文件错误需要通知对面
    {
        if(fd != -1)
        {
            localFile.reset();
            errno = EINVAL;
        }
        perror("open");
        bzero(&train, sizeof(train));
        train.length = 0;
        wire_send_train(netFd, &train, 0);
        return EXIT_FAILURE;
    }
    // 发送文件名(虚拟文件表中存的文件名，客户端下载到本地也用这个名字)；
    if(flock(fd, LOCK_SH | LOCK_NB) == -1)// 避免读取另一个任务正在接收的文件
    {
        int length = 0;
        wire_send_int(netFd, length, MSG_NOSIGNAL);
        return EXIT_FAILURE;
    }
    // 上传完成并且物理文件大小一致时，才能供客户端下载
    int complete;
    int64_t expectedSize;
    struct stat statbuf;
    if(get_local_file_status(md5_str, real_file_name, &complete) != 0 || complete != 1 ||
       get_virtual_file_size(file, user_id, code, &expectedSize) != 0 ||
       fstat(fd, &statbuf) == -1 || statbuf.st_size != expectedSize)
    {
        int length = 0;
        wire_send_int(netFd, length, MSG_NOSIGNAL);
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
    train.length = sizeof(int64_t);
    int64_t fileSize = statbuf.st_size;// 获得文件总大小
    wire_put_i64(train.buf, fileSize);
    if(wire_send_train(netFd, &train, MSG_NOSIGNAL) != (int)sizeof(train.length) + train.length)
    {
        return EXIT_FAILURE;
    }

    train.length = MD5_STR_LEN;
    memcpy(train.buf, md5_str, train.length);
    if(wire_send_train(netFd, &train, MSG_NOSIGNAL) != (int)sizeof(train.length) + train.length)
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
    printf("exitSize = %lld\n", (long long)exitSize);
    printf("fileSize = %lld\n", (long long)fileSize);
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
        while (total < fileSize)        //从total开始传，可能有断点续传
        {
            if (fileSize - total < (int64_t)sizeof(train.buf))//最后一次发送，发送剩余内容
            {
                train.length = fileSize - total;
            }
            else        //发满
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

    localFile.reset();
    int total_time;
    do
    {
        if(wire_recv_int(netFd, &total_time) != sizeof(int))
        {
            return EXIT_FAILURE;
        }
    } while(total_time == TRANSFER_VERIFY_PROGRESS);
    if(total_time < 0)
    {
        return EXIT_FAILURE;
    }
    printf("total time = %ds\n",total_time);
    return 0;
}

int Download(int sockFd, int user_id, int code)
{
    // 先获取文件名，不同客户端传的文件可能文件名相同但文件内容不同，所以根据md5值来判断本地是否有该文件
    char name[1024] = {0};
    int dataLength;
    if(wire_recv_int(sockFd, &dataLength) != sizeof(int))// 文件内容还够时一直将length长度的文件都收完
    {
        return EXIT_FAILURE;
    }
    if(dataLength <= 0 || dataLength >= (int)sizeof(name))
    {
        return EXIT_FAILURE;// 打开文件错误退出
    }
    if(recvn(sockFd, name, dataLength) != dataLength ||
       !is_valid_file_name(name, dataLength))
    {
        return EXIT_FAILURE;
    }
    printf("name = %s\n", name);
    
    // 获取文件大小
    int64_t fileSize;
    if(wire_recv_int(sockFd, &dataLength) != sizeof(int) ||
       dataLength != (int)sizeof(fileSize) ||
       wire_recv_i64(sockFd, &fileSize) != dataLength)
    {
        return EXIT_FAILURE;
    }

    // 获取文件md5值
    char md5_str[MD5_STR_LEN + 1] = {0};
    if(wire_recv_int(sockFd, &dataLength) != sizeof(int) ||
       dataLength != MD5_STR_LEN ||
       recvn(sockFd, md5_str, dataLength) != dataLength)
    {
        return EXIT_FAILURE;
    }
    if(strspn(md5_str, "0123456789abcdefABCDEF") != MD5_STR_LEN)
    {
        return EXIT_FAILURE;
    }

    if(fileSize < 0)
    {
        return EXIT_FAILURE;
    }

    time_t timeBeg, timeEnd;
    timeBeg = time(NULL);// 从检查文件是否存在开始计时

    // 统一准备文件和元数据，成功后保留文件锁供接收或秒传使用
    char real_name[1024] = {0};
    int64_t exitSize = 0;
    char buf[1000] = {0};
    int fd = -1;
    int complete = 0;
    int ret = prepare_upload(code, name, user_id, md5_str, fileSize,
                             real_name, &fd, &complete);
    FileDescriptor localFile(fd);
    if(ret == 1)
    {
        int check = 2;
        wire_send_int(sockFd, check, MSG_NOSIGNAL);
        return EXIT_FAILURE;
    }
    if(ret != 0)
    {
        return EXIT_FAILURE;
    }
    struct stat statbuf;
    if(fstat(fd, &statbuf) == -1)
    {
        perror("fstat");
        return EXIT_FAILURE;
    }
    exitSize = statbuf.st_size;
    if(complete == 1 && exitSize != fileSize)
    {
        if(set_local_file_status(md5_str, real_name, 0) != 0)
        {
            return EXIT_FAILURE;
        }
        complete = 0;
    }
    if(exitSize < 0 || exitSize > fileSize)
    {
        return EXIT_FAILURE;
    }
    int check = complete == 1 ? 0 : 1;
    if(wire_send_int(sockFd, check, MSG_NOSIGNAL) != sizeof(int))
    {
        return EXIT_FAILURE;
    }
    if(check == 0)// 完整文件直接秒传
    {
        timeEnd = time(NULL);
        double elapsed = difftime(timeEnd, timeBeg);
        int total_time = elapsed < 0 ? 0 : elapsed > INT_MAX ? INT_MAX : (int)elapsed;
        printf("total time = %ds\n", total_time);
        return wire_send_int(sockFd, total_time, MSG_NOSIGNAL) == sizeof(int) ? 0 : EXIT_FAILURE;
    }
    // 未完成的满长度文件可能尚未通过校验，重新从头接收
    if(fileSize > 0 && exitSize == fileSize)
    {
        if(ftruncate(fd, 0) == -1)
        {
            perror("ftruncate");
            return EXIT_FAILURE;
        }
        exitSize = 0;
    }
    // 发送本地文件大小
    if(wire_send_i64(sockFd, exitSize, MSG_NOSIGNAL) != sizeof(exitSize))
    {
        return EXIT_FAILURE;
    }
    //偏移到末尾，进行续传
    if(lseek(fd, exitSize, SEEK_SET) == -1)
    {
        return EXIT_FAILURE;
    }

    // 下载文件
    // 判断文件大小，是否使用映射内存接收
    if(fileSize >= 1073741824)// 大于1G，直接接收到mmap映射内存
    {
        if((int64_t)(off_t)fileSize != fileSize || (uint64_t)fileSize > SIZE_MAX ||
           ftruncate(fd, fileSize) == -1)
        {
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
            //先接收本次长度
            ret = wire_recv_int(sockFd, &dataLength);
            if(ret != sizeof(int))// 对面连接断开或长度没有接收完整
            {
                localFile.reset();
                timeEnd = time(NULL);
                int total_time = timeEnd - timeBeg;
                printf("total time = %ds\n", total_time);
                return EXIT_FAILURE;
            }
            if (dataLength == 0 )// 收到0代表传输结束
            {
                if(total != fileSize)
                {
                    return EXIT_FAILURE;
                }
                timeEnd = time(NULL);
                break;
            }
            if(dataLength < 0 || dataLength > (int)sizeof(buf) ||
               dataLength > fileSize - total || recvn(sockFd, buf, dataLength) != dataLength)
            {
                return EXIT_FAILURE;
            }
            //写到文件对象中
            ret = writen(fd, buf, dataLength);
            if(ret != dataLength)
            {
                return EXIT_FAILURE;
            }
            total += dataLength;
        }
    }
    // 在释放文件锁前校验完整文件，包括断点续传前已有的内容
    char actual_md5[MD5_STR_LEN + 1] = {0};
    if(Compute_file_md5_fd(fd, actual_md5, sockFd) != 0)
    {
        int failure = TRANSFER_VERIFY_FAILED;
        wire_send_int(sockFd, failure, MSG_NOSIGNAL);
        return EXIT_FAILURE;
    }
    if(fstat(fd, &statbuf) == -1)
    {
        perror("fstat");
        int failure = TRANSFER_VERIFY_FAILED;
        wire_send_int(sockFd, failure, MSG_NOSIGNAL);
        return EXIT_FAILURE;
    }
    if(statbuf.st_size != fileSize || strcasecmp(actual_md5, md5_str) != 0)
    {
        if(ftruncate(fd, 0) == -1)
        {
            perror("ftruncate");
        }
        int failure = TRANSFER_VERIFY_FAILED;
        wire_send_int(sockFd, failure, MSG_NOSIGNAL);
        return EXIT_FAILURE;
    }
    // 校验通过才记录完成状态，失败时不向客户端报告上传成功
    if(set_local_file_status(md5_str, real_name, 1) != 0)
    {
        int failure = TRANSFER_VERIFY_FAILED;
        wire_send_int(sockFd, failure, MSG_NOSIGNAL);
        return EXIT_FAILURE;
    }
    localFile.reset();
    timeEnd = time(NULL);
    double elapsed = difftime(timeEnd, timeBeg);
    int total_time = elapsed < 0 ? 0 : elapsed > INT_MAX ? INT_MAX : (int)elapsed;
    printf("total time = %ds\n", total_time);
    return wire_send_int(sockFd, total_time, MSG_NOSIGNAL) == sizeof(int) ? 0 : EXIT_FAILURE;
}
