#include "threadPool.h"
// socket bind listen操作
int tcpInit(int *pSockFd, char *file)
{
    //打开config文件
    FILE *fp = fopen(file, "r");
    char ip[1024] = {0};
    char port[1024] = {0};
    char buf[1024] = {0};
    fgets(buf, sizeof(buf), fp);
    int i = 0;
    while (buf[i] < '0' || buf[i] > '9')
    {
        ++i;
    }
    strcpy(ip, buf + i);
    // printf("ip = %s\n", ip);
    //置零
    bzero(buf, sizeof(buf));
    fgets(buf, sizeof(buf), fp) != NULL;
    i = 0;
    while (buf[i] < '0' || buf[i] > '9')
    {
        ++i;
    }
    strcpy(port, buf + i);
    // printf("port = %s\n", port);

    //定义socket，用于充当服务端的监听socket
    *pSockFd = socket(AF_INET, SOCK_STREAM, 0);
    ERROR_CHECK(pSockFd, NULL, "socket");
    struct sockaddr_in addr;    //定义ip:port结构体
    addr.sin_family = AF_INET;  //ipv4协议
    addr.sin_port = htons(atoi(port));  //字符串转数字，再转大端
    addr.sin_addr.s_addr = inet_addr(ip);       //点分十进制转机器数
    int reuse = 1;
    //让bind无视time_wait
    int ret = setsockopt(*pSockFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)); // 设置为忽略time_wait
    ERROR_CHECK(ret, -1, "setsockopt");
    ret = bind(*pSockFd, (struct sockaddr *)&addr, sizeof(addr));
    ERROR_CHECK(ret, -1, "bind");
    //变为监听socket
    listen(*pSockFd, 10);
}
