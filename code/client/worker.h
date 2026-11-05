#ifndef _WORKER_H// 头文件在编译时被多次引用，避免重复定义
#define _WORKER_H
#include<myhead.h>
#include "threadPool.h"
#include <stddef.h>
#define READ_DATA_SIZE 1024
#define MD5_SIZE 16
#define MD5_STR_LEN (MD5_SIZE * 2)
#define SATL_LEN 14
#define LARGE_TRANSFER_SIZE (1024 * 1024)
#define TRANSFER_VERIFY_PROGRESS (-1)
#define TRANSFER_VERIFY_FAILED (-2)
#define KEEPALIVE_INTERVAL_MS 10000
#define TOKEN_LIFETIME 600
#define TOKEN_REFRESH_MARGIN_MS 30000
#define TOKEN_VALID 0
#define TOKEN_INVALID 1
#define TOKEN_EXPIRED 2
#define TOKEN_REFRESH_REPLY 3

// 传输文件用的小火车
typedef struct tain_s{
    int length;
    char buf[1000];
} train_t;

// 传输命令用的小火车
typedef struct tain_state_s{
    int length;
    int state;
    int pre_length;// 如果是注册和登录，用于记录用户名长度，如果是命令，用于记录命令部分长度
    char buf[1000];
} train_state_t;

// 保留现有结构体帧，要求字段布局与原协议一致
typedef char wire_int_width_check[(sizeof(int) == 4 && INT_MAX == INT32_MAX) ? 1 : -1];
typedef char wire_train_layout_check[(offsetof(train_t, buf) == 4) ? 1 : -1];
typedef char wire_command_layout_check[(offsetof(train_state_t, state) == 4 &&
                                        offsetof(train_state_t, pre_length) == 8 &&
                                        offsetof(train_state_t, buf) == 12) ? 1 : -1];

static inline int wire_send_train(int fd, const train_t *train, int flags)
{
    if(train->length < 0 || train->length > (int)sizeof(train->buf))
    {
        errno = EINVAL;
        return -1;
    }
    train_t encoded = *train;
    wire_put_i32(&encoded.length, train->length);
    return network_sendn(fd, &encoded, 4 + train->length, flags);
}

static inline int wire_send_command(int fd, const train_state_t *train, int flags)
{
    if(train->length < 0 || train->length > (int)sizeof(train->buf))
    {
        errno = EINVAL;
        return -1;
    }
    train_state_t encoded = *train;
    wire_put_i32(&encoded.length, train->length);
    wire_put_i32(&encoded.state, train->state);
    wire_put_i32(&encoded.pre_length, train->pre_length);
    return network_sendn(fd, &encoded, 12 + train->length, flags);
}

// 命令枚举
enum command{
    REGISTER,
    LOGIN,
    LS,
    CD,
    PWD,
    REMOVE,
    MKDIR,
    GETS,
    PUTS,
    TOKEN,
    KEEPALIVE,
    REFRESH,
};

int handleCmd(train_state_t train_get, int netFd);
int recvn(int sockFd, void *pstart, int len);
int sendn(int sockFd, const void *pstart, int len, int flags);
int send_keepalive(int sockFd);
int Upload(int netFd, char *file, int fd, const char *md5_str);
int Download(int sockFd);

int register_user(int sockFd);
int login(int sockFd, char *uName);

int Compute_file_md5(char *file_path, char *md5_str);
int Compute_fd_md5(int fd, char *md5_str);

int recvn(int sockFd, void *pstart, int len);
int print_stat(struct stat statbuf, char *name);
int Upload(int netFd, char *file, int fd, const char *md5_str);
int Download(int sockFd);
#endif
