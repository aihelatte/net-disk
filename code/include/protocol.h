#pragma once
#include <climits>
#include <cstddef>
#include "wire_order.h"

// 两端共享同一套帧格式及命令编号。
typedef struct tain_s{
    int length;
    char buf[1000];
} train_t;

// 传输命令用的小火车
typedef struct tain_state_s{
    int length;
    int state;
    int pre_length;// 如果是注册和登录，用于记录用户名长度，如果是命令，用于记录命令部分长度
    char buf[1000];
} train_state_t;

// 保留现有结构体帧，要求字段布局与原协议一致
static_assert(sizeof(int) == 4 && INT_MAX == INT32_MAX);
static_assert(offsetof(train_t, buf) == 4);
static_assert(offsetof(train_state_t, state) == 4 &&
              offsetof(train_state_t, pre_length) == 8 &&
              offsetof(train_state_t, buf) == 12);

static inline int wire_send_train(int fd, const train_t *train, int flags)
{
    if(train->length < 0 || train->length > (int)sizeof(train->buf))
    {
        errno = EINVAL;
        return -1;
    }
    train_t encoded = *train;
    wire_put_i32(&encoded.length, train->length);
    return network_sendn(fd, &encoded, 4 + train->length, flags);
}

static inline int wire_send_command(int fd, const train_state_t *train, int flags)
{
    if(train->length < 0 || train->length > (int)sizeof(train->buf))
    {
        errno = EINVAL;
        return -1;
    }
    train_state_t encoded = *train;
    wire_put_i32(&encoded.length, train->length);
    wire_put_i32(&encoded.state, train->state);
    wire_put_i32(&encoded.pre_length, train->pre_length);
    return network_sendn(fd, &encoded, 12 + train->length, flags);
}

// 命令枚举
enum command{
    REGISTER,
    LOGIN,
    LS,
    CD,
    PWD,
    REMOVE,
    MKDIR,
    GETS,
    PUTS,
    TOKEN,
    KEEPALIVE,
    REFRESH,
};
