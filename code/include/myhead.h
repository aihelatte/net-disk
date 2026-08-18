#ifndef NET_DISK_MYHEAD_H
#define NET_DISK_MYHEAD_H

#include <arpa/inet.h>
#include <crypt.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mysql/mysql.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ERROR_CHECK(retval, target, message) \
    do {                                      \
        if ((retval) == (target)) {           \
            perror(message);                  \
            return -1;                        \
        }                                     \
    } while (0)

#endif
