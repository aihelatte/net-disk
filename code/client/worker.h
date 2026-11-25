#ifndef _WORKER_H// 头文件在编译时被多次引用，避免重复定义
#define _WORKER_H
#include<myhead.h>
#include "protocol.h"
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



int handleCmd(train_state_t train_get, int netFd);
int recvn(int sockFd, void *pstart, int len);
int sendn(int sockFd, const void *pstart, int len, int flags);
int send_keepalive(int sockFd);
int Upload(int netFd, char *file, int fd, const char *md5_str);
int Download(int sockFd);

int register_user(int sockFd);
int login(int sockFd, char *uName);

int Compute_file_md5(char *file_path, char *md5_str);
int Compute_fd_md5(int fd, char *md5_str, int sockFd = -1);

int print_stat(struct stat statbuf, char *name);
#endif
