#ifndef _WORKER_H// 头文件在编译时被多次引用，避免重复定义
#define _WORKER_H
#include<myhead.h>
#include <stddef.h>
#define READ_DATA_SIZE 1024
#define MD5_SIZE 16
#define MD5_STR_LEN (MD5_SIZE * 2)
#define SATL_LEN 14// 盐值长度
#define TIME_SLICE 3000// 每格100ms，一轮约5分钟
#define LARGE_TRANSFER_SIZE (1024 * 1024)
#define TRANSFER_VERIFY_PROGRESS (-1)
#define TRANSFER_VERIFY_FAILED (-2)
#define TOKEN_LIFETIME 600
#define TOKEN_VALID 0
#define TOKEN_INVALID 1
#define TOKEN_EXPIRED 2
#define TOKEN_REFRESH_REPLY 3
#define TOKEN_SESSION_LENGTH 64
#define MAX_CONTROL_REPLY (4 * 1024 * 1024)// 单个控制回复上限，超出则关闭连接
//tlv协议数据单元
typedef struct tain_s{
    int length;
    char buf[1000];
} train_t;

//tlv协议数据单元，命令版
typedef struct tain_state_s{
    int length;
    int state;
    int pre_length;// 如果是注册和登录，用于记录用户名长度，如果是命令，用于记录命令部分长度
    char buf[1000];
} train_state_t;

// 保留现有结构体帧，要求字段布局与原协议一致
typedef char wire_int_width_check[(sizeof(int) == 4 && INT_MAX == INT32_MAX) ? 1 : -1];
typedef char wire_train_layout_check[(offsetof(train_t, buf) == 4) ? 1 : -1];
typedef char wire_command_layout_check[(offsetof(train_state_t, state) == 4 &&
                                        offsetof(train_state_t, pre_length) == 8 &&
                                        offsetof(train_state_t, buf) == 12) ? 1 : -1];

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

// 主线程只保存收发进度，不等待某个连接把整条消息发完
enum receive_stage{
    RECEIVE_COMMAND,
    RECEIVE_REGISTER_PASSWORD,
    RECEIVE_LOGIN_PASSWORD,
};

typedef struct connection_s{
    int netFd;
    int stage;
    int received;
    int needed;
    train_state_t train;
    char user_name[PROTOCOL_USER_NAME_CAPACITY];
    char sid[TOKEN_SESSION_LENGTH + 1];
    char salt[SATL_LEN + 1];
    char password[50];
    int auth_check;
    int transfer_user_id;
    char *sendBuf;
    size_t sendSize;
    size_t sendOffset;
} connection_t;

//集合里面的每一个元素结构体
typedef struct slotNode_s {
    int netFd;
    struct slotNode_s* next;
} slotNode_t;

//循环队列里面的集合结构体
typedef struct slotList_s {
    slotNode_t* head;
    slotNode_t* tail;
    int size;
} slotList_t;

typedef struct clientQueue_s{// 已连接客户队列
    int *client;// 用于记录每个客户端的user_id
    connection_t **connections;// 仅主线程持有，数据连接交接时释放
    slotList_t time_out[TIME_SLICE];// 循环队列，服务于时间轮转算法
    int *index;// 每个netFd当前在轮盘循环队列的位置，下标netFd对应
    size_t fd_capacity;// client和index能够容纳的fd数量
    int timer;// 记录当前所在循环队列位置的下标
} clientQueue_t;



// 客户端队列的操作
int init_clientQueue(clientQueue_t *pclientQueue);
int fdAdd(int netFd, clientQueue_t *pclientQueue);
int fdDel(int netFd, clientQueue_t *pclientQueue);

// 用户注册登录
int register_user(connection_t *pConnection);
int login(connection_t *pConnection, clientQueue_t *pclientQueue);
int token(connection_t *pConnection, clientQueue_t *pclientQueue);
int refresh_token(connection_t *pConnection);
int init_token_key();
int make_session_id(char *sid);
int encode(connection_t *pConnection, char *token);
int decode(char *user_name, char *token, clientQueue_t *pclientQueue, int *puser_id);

// 处理短命令
int queue_reply(connection_t *pConnection, const void *buf, int length);
static inline int wire_queue_int(connection_t *pConnection, int value)
{
    unsigned char data[4];
    wire_put_i32(data, value);
    return queue_reply(pConnection, data, sizeof(data));
}

static inline int wire_queue_train(connection_t *pConnection, const train_t *train)
{
    if(train->length < 0 || train->length > (int)sizeof(train->buf))
    {
        errno = EINVAL;
        return -1;
    }
    train_t encoded = *train;
    wire_put_i32(&encoded.length, train->length);
    return queue_reply(pConnection, &encoded, 4 + train->length);
}

int flush_reply(connection_t *pConnection);
int handle_short_cmd(train_state_t train_get, connection_t *pConnection, int user_id);
int recvn(int sockFd, void *pstart, int len);
int sendn(int sockFd, const void *pstart, int len, int flags);

// 处理长命令
int handle_long_cmd(int netFd, int user_id);
int Upload(int netFd, char *file, int user_id);
int Download(int sockFd, int user_id);

int Compute_file_md5(char *file_path, char *md5_str);// 生成文件的md5码
int Compute_file_md5_fd(int fd, char *md5_str, int sockFd);

// 数据库基本连接和执行操作
int mysqlconnect(MYSQL **conn);
int do_query(char *query, MYSQL **conn);
int add_user(char *user_name, char *salt, char *crptpasswd);

// 操作用户信息表
int find_user_id(char *user_name, int *puser_id);
int get_user_salt(char *user_name, char *salt, char *crptpasswd);
int get_user_name(char *user_name, const int user_id);
int get_user_code(int user_id, int *pcode);
int get_user_pwd(int user_id, char *pwd);
int update_user_pwd(int user_id, char *pwd, int code);

// 操作虚拟文件表
int init_local_file();
int add_link(char *md5_str);// 增加链接数
int minus_link(char *md5_str);// 减少链接数
int get_virtual_file_md5(char *file, int user_id, int parent_id, char *md5_str);// 获取文件md5值，也可以检查文件是否存在，存在返回0，不存在返回1
int get_virtual_file_size(char *file, int user_id, int parent_id, int64_t *size);
int get_real_file_name(char *md5_str, char *real_file_name);// 获取文件本地真实名称
int get_local_file_status(char *md5_str, char *real_name, int *complete);
int set_local_file_status(char *md5_str, char *real_name, int complete);
int get_file_code(int user_id, char *filename, int *pcode, int parent_id, char *type);
int get_parent_id(int id, int *pparent_code);
int add_local_file(char *md5_str, char *real_name);// 在本地文件表中插入新数据
int prepare_upload(int parent_id, char *name, int owner_id, char *md5_str,
                   int64_t fileSize, char *real_name, int *pfd, int *complete);
int add_virtual_dir(int parent_id, char *filename, int owner_id);
int add_virtual_file(int parent_id, char *filename, int owner_id, char *md5_str, int64_t filesize);
int del_virtual_file(int code, int user_id);// 删除虚拟文件/目录
int if_local_exist(char *md5_str);
int find_code_files(connection_t *pConnection, int code, int user_id);


// 日志记录
int sysadd(int user_id, char *cmd);

#endif
