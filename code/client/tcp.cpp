#include "threadPool.h"
// connect
int tcpInit(int *pSockFd, const char *file)
{
    struct sockaddr_in addr;
    if(read_tcp_address(file, &addr) != 0)
    {
        return -1;
    }
    int ret = connect(*pSockFd, (struct sockaddr *)&addr, sizeof(addr));
    ERROR_CHECK(ret, -1, "connect");
    return 0;
}
