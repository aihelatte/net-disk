#include "threadPool.h"

// 在调用方持锁时复制并入队一条传输命令。分配失败返回 -1。
int TaskQueue::push(const TransferTask &task)
{
    try
    {
        tasks.push(task);
    } catch(const std::bad_alloc &) {
        return -1;
    }
    return 0;
}
