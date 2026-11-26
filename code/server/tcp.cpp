#include "file_descriptor.h"
#include "threadPool.h"
// socket bind listen操作
int tcpInit(int *pSockFd, const char *file)
{
    struct sockaddr_in addr;
    if (read_tcp_address(file, &addr) != 0)
    {
        return -1;
    }
    *pSockFd = -1;
    FileDescriptor socketFd(socket(AF_INET, SOCK_STREAM, 0));
    ERROR_CHECK(socketFd.get(), -1, "socket");
    int reuse = 1;
    if (setsockopt(socketFd.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1 ||
       bind(socketFd.get(), (struct sockaddr *)&addr, sizeof(addr)) == -1 ||
       listen(socketFd.get(), 10) == -1)
    {
        perror("tcpInit");
        return -1;
    }
    *pSockFd = socketFd.release();
    return 0;
}
