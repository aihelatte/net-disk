#include "threadPool.h"

int TaskQueue::push(const TransferTask &task)
{
    try {
        tasks.push(task);
    } catch(const std::bad_alloc &) {
        return -1;
    }
    return 0;
}
