#ifndef NET_DISK_WIRE_ORDER_H
#define NET_DISK_WIRE_ORDER_H

#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include "net_io.h"

// 协议中的整数使用网络字节序，原始文件和字符串不转换
static inline void wire_put_i32(void *dst, int value)
{
    uint32_t encoded = htonl((uint32_t)value);
    memcpy(dst, &encoded, sizeof(encoded));
}

static inline int wire_get_i32(const void *src)
{
    uint32_t encoded;
    memcpy(&encoded, src, sizeof(encoded));
    uint32_t value = ntohl(encoded);
    if(value <= INT32_MAX)
    {
        return (int)value;
    }
    return (int)(-1 - (int64_t)(UINT32_MAX - value));
}

static inline void wire_put_i64(void *dst, int64_t value)
{
    unsigned char *p = (unsigned char *)dst;
    uint64_t bits = (uint64_t)value;
    for(int i = 7; i >= 0; --i)
    {
        p[i] = (unsigned char)(bits & 0xff);
        bits >>= 8;
    }
}

static inline int64_t wire_get_i64(const void *src)
{
    const unsigned char *p = (const unsigned char *)src;
    uint64_t bits = 0;
    for(int i = 0; i < 8; ++i)
    {
        bits = (bits << 8) | p[i];
    }
    if(bits <= INT64_MAX)
    {
        return (int64_t)bits;
    }
    return -1 - (int64_t)(UINT64_MAX - bits);
}

static inline int wire_send_int(int fd, int value, int flags)
{
    unsigned char data[4];
    wire_put_i32(data, value);
    return network_sendn(fd, data, sizeof(data), flags);
}

static inline int wire_recv_int(int fd, int *value)
{
    unsigned char data[4];
    int ret = network_recvn(fd, data, sizeof(data));
    if(ret == sizeof(data))
    {
        *value = wire_get_i32(data);
    }
    return ret;
}

static inline int wire_send_i64(int fd, int64_t value, int flags)
{
    unsigned char data[8];
    wire_put_i64(data, value);
    return network_sendn(fd, data, sizeof(data), flags);
}

static inline int wire_recv_i64(int fd, int64_t *value)
{
    unsigned char data[8];
    int ret = network_recvn(fd, data, sizeof(data));
    if(ret == sizeof(data))
    {
        *value = wire_get_i64(data);
    }
    return ret;
}

#endif
