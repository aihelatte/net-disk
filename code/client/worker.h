#ifndef _WORKER_H// 头文件在编译时被多次引用，避免重复定义
#define _WORKER_H
#include<myhead.h>
#include "protocol.h"
inline constexpr int READ_DATA_SIZE = 1024;
inline constexpr int MD5_SIZE = 16;
inline constexpr int MD5_STR_LEN = (MD5_SIZE * 2);
inline constexpr int SATL_LEN = 14;
inline constexpr int LARGE_TRANSFER_SIZE = (1024 * 1024);
inline constexpr int TRANSFER_VERIFY_PROGRESS = (-1);
inline constexpr int TRANSFER_VERIFY_FAILED = (-2);
inline constexpr int KEEPALIVE_INTERVAL_MS = 10000;
inline constexpr int TOKEN_LIFETIME = 600;
inline constexpr int TOKEN_REFRESH_MARGIN_MS = 30000;
inline constexpr int TOKEN_VALID = 0;
inline constexpr int TOKEN_INVALID = 1;
inline constexpr int TOKEN_EXPIRED = 2;
inline constexpr int TOKEN_REFRESH_REPLY = 3;



int recvn(int sockFd, void *pstart, int len);
int send_keepalive(int sockFd);
int Upload(int netFd, char *file, int fd, const char *md5_str);
int Download(int sockFd);

int register_user(int sockFd);
int login(int sockFd, char *uName);

int Compute_fd_md5(int fd, char *md5_str, int sockFd = -1);

#endif
