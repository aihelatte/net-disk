#ifndef NET_DISK_PROTOCOL_VALIDATE_H
#define NET_DISK_PROTOCOL_VALIDATE_H

#include <stddef.h>

#define PROTOCOL_USER_NAME_CAPACITY 100
#define PROTOCOL_COMMAND_CAPACITY 100
#define PROTOCOL_TOKEN_CAPACITY 1024

static inline int protocol_valid_text_length(int length, size_t capacity)
{
    return length >= 0 && (size_t)length < capacity;
}

static inline int protocol_valid_pre_length(int length, int pre_length)
{
    return pre_length >= 0 && pre_length <= length;
}

static inline int protocol_valid_split_text(int length,
                                            int pre_length,
                                            size_t first_capacity,
                                            size_t second_capacity)
{
    if(!protocol_valid_pre_length(length, pre_length) ||
       pre_length == 0 || pre_length == length)
    {
        return 0;
    }

    return protocol_valid_text_length(pre_length, first_capacity) &&
           protocol_valid_text_length(length - pre_length, second_capacity);
}

#endif
