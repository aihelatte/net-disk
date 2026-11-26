#ifndef _WORKER_H// 头文件在编译时被多次引用，避免重复定义
#define _WORKER_H
#include<myhead.h>
#include "protocol.h"
#include "file_descriptor.h"
#include <utility>
#include <vector>
#include <list>
#include <memory>
inline constexpr int READ_DATA_SIZE = 1024;
inline constexpr int MD5_SIZE = 16;
inline constexpr int MD5_STR_LEN = (MD5_SIZE * 2);
inline constexpr int SATL_LEN = 14;  // 盐值长度
inline constexpr int TIME_SLICE = 3000;  // 每格100ms，一轮约5分钟
inline constexpr int LARGE_TRANSFER_SIZE = (1024 * 1024);
inline constexpr int TRANSFER_VERIFY_PROGRESS = (-1);
inline constexpr int TRANSFER_VERIFY_FAILED = (-2);
inline constexpr int TOKEN_LIFETIME = 600;
inline constexpr int TOKEN_VALID = 0;
inline constexpr int TOKEN_INVALID = 1;
inline constexpr int TOKEN_EXPIRED = 2;
inline constexpr int TOKEN_REFRESH_REPLY = 3;
inline constexpr int TOKEN_SESSION_LENGTH = 64;
inline constexpr int MAX_CONTROL_REPLY = (4 * 1024 * 1024);  // 单个控制回复上限，超出则关闭连接


// 主线程只保存收发进度，不等待某个连接把整条消息发完
enum receive_stage{
    RECEIVE_COMMAND,
    RECEIVE_REGISTER_PASSWORD,
    RECEIVE_LOGIN_PASSWORD,
};

// 持有一个套接字及其认证、会话目录、分帧接收和待发回复状态。
class Connection
{
public:
    explicit Connection(FileDescriptor socket) noexcept : socket_(std::move(socket)) {}
    int fd() const noexcept { return socket_.get(); }
    FileDescriptor &socketForHandoff() noexcept { return socket_; }
    bool hasPartialRequest() const noexcept { return received != 0; }
    void resetRequest();
    int receiveRequest(bool &progress);
    bool hasReply() const { return !sendBuf.empty(); }
    int queueReply(const void *buf, int length);
    int flushReply();

    receive_stage stage = RECEIVE_COMMAND;
    train_state_t train{};
    char user_name[PROTOCOL_USER_NAME_CAPACITY]{};
    char sid[TOKEN_SESSION_LENGTH + 1]{};
    char salt[SATL_LEN + 1]{};
    char password[50]{};
    int auth_check = 0;
    int userId = -1;
    int transfer_user_id = -1;
    int directoryId = 0;
    char directoryPath[sizeof(train_t::buf)] = "~/";
private:
    bool validHeader() const;
    bool validBody() const;
    FileDescriptor socket_;
    int received = 0;
    int needed = sizeof(int);
    std::vector<char> sendBuf;
    size_t sendOffset = 0;
};

// 服务端主线程管理活跃连接和时间轮。按 fd 查找连接，并保存对应的槽位迭代器。
class ClientManager
{
public:
    int initialize();
    int add(FileDescriptor socket);
    Connection *find(int fd);
    const Connection *find(int fd) const;
    void touch(int fd);
    void remove(int fd);
    int nextExpired() const;
    void advanceTimer();
private:
    struct Entry
    {
        explicit Entry(FileDescriptor socket) : connection(std::move(socket)) {}
        Connection connection;
        int slot = 0;
        std::list<int>::iterator position;
    };
    std::vector<std::unique_ptr<Entry>> clients;
    std::list<int> wheel[TIME_SLICE];
    int timer = 0;
};

// 用户注册登录
int register_user(Connection *pConnection);
int login(Connection *pConnection);
int token(Connection *pConnection, ClientManager *clients);
int refresh_token(Connection *pConnection);
int init_token_key();
int make_session_id(char *sid);
int encode(Connection *pConnection, char *token);
int decode(char *user_name, char *token, const ClientManager *clients, int *puser_id);

// 处理短命令
static inline int wire_queue_int(Connection *pConnection, int value)
{
    unsigned char data[4];
    wire_put_i32(data, value);
    return pConnection->queueReply(data, sizeof(data));
}

static inline int wire_queue_train(Connection *pConnection, const train_t *train)
{
    if (train->length < 0 || train->length > (int)sizeof(train->buf))
    {
        errno = EINVAL;
        return -1;
    }
    train_t encoded = *train;
    wire_put_i32(&encoded.length, train->length);
    return pConnection->queueReply(&encoded, 4 + train->length);
}

int handle_short_cmd(train_state_t train_get, Connection *pConnection, int user_id);
int recvn(int sockFd, void *pstart, int len);

// 处理长命令
int handle_long_cmd(int netFd, int user_id);
int Upload(int netFd, char *file, int user_id, int code);
int Download(int sockFd, int user_id, int code);

int Compute_file_md5(char *file_path, char *md5_str);// 生成文件的md5码
int Compute_file_md5_fd(int fd, char *md5_str, int sockFd);

int add_user(char *user_name, char *salt, char *crptpasswd);

// 操作用户信息表
int find_user_id(char *user_name, int *puser_id);
int get_user_salt(char *user_name, char *salt, char *crptpasswd);
int get_user_name(char *user_name, const int user_id);

// 操作虚拟文件表
int init_local_file();
int get_virtual_file_md5(char *file, int user_id, int parent_id, char *md5_str);// 获取文件md5值，也可以检查文件是否存在，存在返回0，不存在返回1
int get_virtual_file_size(char *file, int user_id, int parent_id, int64_t *size);
int get_real_file_name(char *md5_str, char *real_file_name);// 获取文件本地真实名称
int get_local_file_status(char *md5_str, char *real_name, int *complete);
int set_local_file_status(char *md5_str, char *real_name, int complete);
int get_file_code(int user_id, const char *filename, int *pcode, int parent_id, char *type);
int get_parent_id(int id, int *pparent_code);
int prepare_upload(int parent_id, char *name, int owner_id, char *md5_str,
                   int64_t fileSize, char *real_name, int *pfd, int *complete);
int add_virtual_dir(int parent_id, const char *filename, int owner_id);
int del_virtual_file(int code, int user_id);// 删除虚拟文件/目录
int if_local_exist(char *md5_str);
int find_code_files(Connection *pConnection, int code, int user_id);


// 日志记录
int sysadd(int user_id, const char *cmd);

#endif
