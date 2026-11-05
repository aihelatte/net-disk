#include"threadPool.h"
int epollAdd(int fd, int epfd)
{
    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = fd;
    int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event);
    ERROR_CHECK(ret, -1, "epoll_ctl");
    return 0;
}
int epollDel(int fd, int epfd)
{
    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = fd;
    int ret = epoll_ctl(epfd, EPOLL_CTL_DEL, fd, &event);
    ERROR_CHECK(ret, -1, "epoll_ctl");
    return 0;
}


int epollMod(int fd, int epfd, uint32_t events)
{
    struct epoll_event event;
    event.events = events;
    event.data.fd = fd;
    int ret = epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &event);
    ERROR_CHECK(ret, -1, "epoll_ctl");
    return 0;
}
