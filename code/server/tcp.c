#include "threadPool.h"
// socket bind listen操作
int tcpInit(int *pSockFd, char *file)
{
    struct sockaddr_in addr;
    if(read_tcp_address(file, &addr) != 0)
    {
        return -1;
    }
    *pSockFd = socket(AF_INET, SOCK_STREAM, 0);
    ERROR_CHECK(*pSockFd, -1, "socket");
    int reuse = 1;
    if(setsockopt(*pSockFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1 ||
       bind(*pSockFd, (struct sockaddr *)&addr, sizeof(addr)) == -1 ||
       listen(*pSockFd, 10) == -1)
    {
        perror("tcpInit");
        close(*pSockFd);
        *pSockFd = -1;
        return -1;
    }
    return 0;
}
