#ifndef NET_DISK_MYHEAD_H
#define NET_DISK_MYHEAD_H

#include <arpa/inet.h>
#include <crypt.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mysql/mysql.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <memory>
#include <utility>
#include "file_descriptor.h"
#include "protocol_validate.h"
#include "net_io.h"
#include "wire_order.h"

namespace {
struct FileCloser
{
    void operator()(FILE *file) const
    {
        fclose(file);
    }
};
}

#define ERROR_CHECK(retval, target, message) \
    do {                                      \
        if ((retval) == (target)) {           \
            perror(message);                  \
            return -1;                        \
        }                                     \
    } while (0)

// 读取一整行，超长时丢弃剩余输入，不把截断内容当成有效命令
static inline int read_input_line(char *buf, size_t capacity)
{
    if(buf == NULL || capacity == 0)
    {
        errno = EINVAL;
        return -1;
    }
    size_t length = 0;
    int tooLong = 0;
    int extraCR = 0;
    char ch;
    while(1)
    {
        ssize_t ret = read(STDIN_FILENO, &ch, 1);
        if(ret == -1 && errno == EINTR)
        {
            continue;
        }
        if(ret <= 0)
        {
            return -1;
        }
        if(ch == '\n')
        {
            break;
        }
        if(extraCR)
        {
            tooLong = 1;
        }
        if(ch == '\r' && length + 1 == capacity && !extraCR)
        {
            extraCR = 1;
            continue;
        }
        if(ch == '\0' || length + 1 >= capacity)
        {
            tooLong = 1;
        }
        else
        {
            buf[length++] = ch;
        }
    }
    if(length > 0 && buf[length - 1] == '\r')
    {
        --length;
    }
    buf[length] = '\0';
    return tooLong ? -2 : (int)length;
}

// 保留原配置的两行格式，缺项、非法IP或端口均返回失败
static inline int read_tcp_address(const char *file, struct sockaddr_in *addr)
{
    std::unique_ptr<FILE, FileCloser> fp(fopen(file, "r"));
    if(!fp)
    {
        perror("fopen");
        return -1;
    }
    char ip[1024] = {0};
    char port[1024] = {0};
    int valid = fgets(ip, sizeof(ip), fp.get()) != NULL &&
                fgets(port, sizeof(port), fp.get()) != NULL;
    valid = valid && strchr(ip, '\n') != NULL && strlen(port) < sizeof(port) - 1;
    fp.reset();
    char *pIp = strchr(ip, '=');
    pIp = pIp == NULL ? ip : pIp + 1;
    pIp += strspn(pIp, " \t");
    char *pPort = strchr(port, '=');
    pPort = pPort == NULL ? port : pPort + 1;
    pPort += strspn(pPort, " \t");
    char *ipEnd = pIp + strcspn(pIp, " \t\r\n");
    valid = valid && ipEnd[strspn(ipEnd, " \t\r\n")] == '\0';
    *ipEnd = '\0';
    char *end;
    errno = 0;
    long portNum = strtol(pPort, &end, 10);
    end += strspn(end, " \t\r\n");
    bzero(addr, sizeof(*addr));
    if(!valid || pPort == end || errno != 0 || *end != '\0' ||
       portNum <= 0 || portNum > 65535 || inet_pton(AF_INET, pIp, &addr->sin_addr) != 1)
    {
        fprintf(stderr, "Invalid TCP configuration.\n");
        return -1;
    }
    addr->sin_family = AF_INET;
    addr->sin_port = htons(portNum);
    return 0;
}

// 协议中只传文件名，本地相对路径交给open_in_root处理
static inline int is_valid_file_name(const char *name, int length)
{
    if(length <= 0 || length > NAME_MAX ||
       (length == 1 && name[0] == '.') ||
       (length == 2 && name[0] == '.' && name[1] == '.'))
    {
        return 0;
    }
    for(int i = 0; i < length; ++i)
    {
        unsigned char ch = name[i];
        if(ch < 32 || ch == 127 || ch == '/' || ch == '\\')
        {
            return 0;
        }
    }
    return 1;
}

// 从指定目录逐段打开文件，保留.和..，但不能退到根目录之外
static inline int open_in_root(const char *root, const char *file, int flags, mode_t mode)
{
    char path[1024] = {0};
    if(file == NULL || file[0] == '\0' || file[0] == '/')
    {
        errno = EINVAL;
        return -1;
    }
    size_t length = strnlen(file, sizeof(path));
    if(length >= sizeof(path))
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    if(file[length - 1] == '/')
    {
        errno = EISDIR;
        return -1;
    }
    for(size_t i = 0; i < length; ++i)
    {
        unsigned char ch = file[i];
        if(ch < 32 || ch == 127 || ch == '\\')
        {
            errno = EINVAL;
            return -1;
        }
    }
    memcpy(path, file, length + 1);

    FileDescriptor dirFd(open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if(dirFd.get() == -1)
    {
        return -1;
    }
    FileDescriptor parentFd[sizeof(path) / 2];
    int depth = 0;
    FileDescriptor fd;
    char *save;
    char *part = strtok_r(path, "/", &save);
    while(part != NULL)
    {
        char *next = strtok_r(NULL, "/", &save);
        if(strcmp(part, ".") == 0)
        {
            if(next == NULL)
            {
                errno = EISDIR;
                break;
            }
        }
        else if(strcmp(part, "..") == 0)
        {
            if(depth == 0)
            {
                errno = EACCES;
                break;
            }
            dirFd = std::move(parentFd[--depth]);
            if(next == NULL)
            {
                errno = EISDIR;
                break;
            }
        }
        else if(next == NULL)
        {
            // 先避免特殊文件阻塞，再检查是否为普通文件
            fd.reset(openat(dirFd.get(), part, flags | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, mode));
            break;
        }
        else
        {
            if(depth >= (int)(sizeof(parentFd) / sizeof(parentFd[0])))
            {
                errno = ENAMETOOLONG;
                break;
            }
            FileDescriptor nextFd(openat(dirFd.get(), part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if(nextFd.get() == -1)
            {
                break;
            }
            // 下钻前保存当前目录，处理 .. 时可以从栈中取回。
            parentFd[depth++] = std::move(dirFd);
            dirFd = std::move(nextFd);
        }
        part = next;
    }
    if(fd.get() != -1)
    {
        struct stat statbuf;
        if(fstat(fd.get(), &statbuf) == -1)
        {
            int saved_errno = errno;
            fd.reset();
            errno = saved_errno;
        }
        else if(!S_ISREG(statbuf.st_mode))
        {
            fd.reset();
            errno = EINVAL;
        }
    }
    int saved_errno = errno;
    dirFd.reset();
    while(depth > 0)
    {
        parentFd[--depth].reset();
    }
    errno = saved_errno;
    // 把结果 fd 交给调用方管理，各级目录 fd 已经关闭。
    return fd.release();
}

#endif
