#include "threadPool.h"
#include "worker.h"
#include "md5.h"

// 使用已有文件描述符读取完整文件，不改变调用方的文件偏移
int Compute_file_md5_fd(int fd, char *md5_str, int sockFd)
{
    unsigned char data[64 * 1024];
    unsigned char md5_value[MD5_SIZE];
    MD5_CTX md5;
    off_t offset = 0;
    int64_t lastProgress = 0;
    if(sockFd >= 0)
    {
        lastProgress = network_time_ms();
        if(lastProgress < 0)
        {
            return -1;
        }
    }

    MD5Init(&md5);
    while(1)
    {
        ssize_t ret = pread(fd, data, sizeof(data), offset);
        if(ret == -1)
        {
            if(errno == EINTR)
            {
                continue;
            }
            perror("pread");
            return -1;
        }
        if(ret == 0)
        {
            break;
        }
        MD5Update(&md5, data, ret);
        offset += ret;
        if(sockFd >= 0)
        {
            int64_t now = network_time_ms();
            if(now < 0)
            {
                return -1;
            }
            if(now - lastProgress >= 10000)
            {
                int progress = TRANSFER_VERIFY_PROGRESS;
                if(wire_send_int(sockFd, progress, MSG_NOSIGNAL) != sizeof(progress))
                {
                    return -1;
                }
                lastProgress = now;
            }
        }
    }

    MD5Final(&md5, md5_value);
    for(int i = 0; i < MD5_SIZE; ++i)
    {
        snprintf(md5_str + i * 2, 3, "%02x", md5_value[i]);
    }
    md5_str[MD5_STR_LEN] = '\0';
    return 0;
}

int Compute_file_md5(char *file_path, char *md5_str)
{
    int fd = open_in_root("NetDisk", file_path, O_RDONLY, 0);
    if(fd == -1)
    {
        perror("open");
        return -1;
    }
    int ret = Compute_file_md5_fd(fd, md5_str, -1);
    close(fd);
    return ret;
}
