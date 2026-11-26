#include "worker.h"
#include <sys/resource.h>

// 按进程的 fd 上限分配连接表。启动时调用，成功返回 0。
int ClientManager::initialize()
{
    struct rlimit fdLimit;
    if (getrlimit(RLIMIT_NOFILE, &fdLimit) == -1)
    {
        perror("getrlimit");
        return -1;
    }
    if (fdLimit.rlim_cur == RLIM_INFINITY || fdLimit.rlim_cur == 0 ||
       fdLimit.rlim_cur > clients.max_size())
    {
        fprintf(stderr, "Invalid file descriptor limit.\n");
        return -1;
    }
    try
    {
        clients.resize(static_cast<size_t>(fdLimit.rlim_cur));
    } catch(const std::bad_alloc &) {
        return -1;
    }
    return 0;
}

// 登记新套接字并安排超时槽位。失败返回 -1，由当前所有者释放套接字。
int ClientManager::add(FileDescriptor socket)
{
    int fd = socket.get();
    if (fd < 0 || static_cast<size_t>(fd) >= clients.size() || clients[fd])
    {
        return -1;
    }
    int slot = (timer + TIME_SLICE - 1) % TIME_SLICE;
    try
    {
        // Entry 接管套接字。时间轮节点插入成功后，才把 Entry 放入连接表。
        auto entry = std::make_unique<Entry>(std::move(socket));
        entry->slot = slot;
        entry->position = wheel[slot].insert(wheel[slot].end(), fd);
        clients[fd] = std::move(entry);
    } catch(const std::bad_alloc &) {
        return -1;
    }
    return 0;
}

Connection *ClientManager::find(int fd)
{
    if (fd < 0 || static_cast<size_t>(fd) >= clients.size() || !clients[fd])
    {
        return nullptr;
    }
    return &clients[fd]->connection;
}

const Connection *ClientManager::find(int fd) const
{
    if (fd < 0 || static_cast<size_t>(fd) >= clients.size() || !clients[fd])
    {
        return nullptr;
    }
    return &clients[fd]->connection;
}

// 连接有收发进度时刷新超时位置。无效 fd 或已移除的连接直接忽略。
void ClientManager::touch(int fd)
{
    if (fd < 0 || static_cast<size_t>(fd) >= clients.size() || !clients[fd])
    {
        return;
    }
    Entry &entry = *clients[fd];
    int slot = (timer + TIME_SLICE - 1) % TIME_SLICE;
    if (entry.slot != slot)
    {
        // 移动现有节点不分配内存，原迭代器在新槽里仍然有效。
        wheel[slot].splice(wheel[slot].end(), wheel[entry.slot], entry.position);
        entry.slot = slot;
    }
}

// 用保存的迭代器删除超时节点，再释放连接表中的对应对象。
void ClientManager::remove(int fd)
{
    if (fd < 0 || static_cast<size_t>(fd) >= clients.size() || !clients[fd])
    {
        return;
    }
    std::unique_ptr<Entry> &entry = clients[fd];
    wheel[entry->slot].erase(entry->position);
    entry.reset();
}

// 每次只取当前槽位的首个过期 fd。槽位空时返回 -1，主循环随后推进一格。
int ClientManager::nextExpired() const
{
    return wheel[timer].empty() ? -1 : wheel[timer].front();
}

void ClientManager::advanceTimer()
{
    timer = (timer + 1) % TIME_SLICE;
}
