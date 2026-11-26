#include "worker.h"
int make_salt(char *salt)
{
    static const char chars[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    FileDescriptor fd(open("/dev/urandom", O_RDONLY | O_CLOEXEC));
    if (fd.get() == -1)
    {
        return -1;
    }
    unsigned char random[SATL_LEN - 3];
    size_t total = 0;
    while (total < sizeof(random))
    {
        ssize_t ret = read(fd.get(), random + total, sizeof(random) - total);
        if (ret == -1 && errno == EINTR)
        {
            continue;
        }
        if (ret <= 0)
        {
            return -1;
        }
        total += ret;
    }

    memcpy(salt, "$6$", 3);
    for (int i = 3; i < SATL_LEN; ++i)
    {
        salt[i] = chars[random[i - 3] & 63];
    }
    salt[SATL_LEN] = '\0';
    return 0;
}

// 第一次处理用户名并排队发送盐值；密码到齐后再次进入此函数
int register_user(Connection *pConnection)
{
    if (pConnection->stage == RECEIVE_COMMAND)
    {
        strcpy(pConnection->user_name, pConnection->train.buf);
        int user_id;
        int ret = find_user_id(pConnection->user_name, &user_id);
        if (ret < 0)
        {
            return -1;
        }
        pConnection->auth_check = ret == 0 ? 2 : 0;
        if (make_salt(pConnection->salt) != 0)
        {
            return -1;
        }
        train_t train;
        train.length = strlen(pConnection->salt);
        strcpy(train.buf, pConnection->salt);
        if (wire_queue_train(pConnection, &train) !=
           (int)(sizeof(train.length) + train.length))
        {
            return -1;
        }
        pConnection->stage = RECEIVE_REGISTER_PASSWORD;
        return 0;
    }

    int check = pConnection->auth_check;
    char crptpasswd[50] = {0};
    // 密码帧只有length和正文，不含命令帧的state、pre_length
    memcpy(crptpasswd, (char *)&pConnection->train + sizeof(int), 49);
    if (check == 0)
    {
        if (add_user(pConnection->user_name, pConnection->salt, crptpasswd) != 0)
        {
            check = 1;
        }
        else
        {
            int user_id;
            if (find_user_id(pConnection->user_name, &user_id) != 0)
            {
                return -1;
            }
            sysadd(user_id, "registed!");
        }
    }
    pConnection->stage = RECEIVE_COMMAND;
    return wire_queue_int(pConnection, check) == sizeof(check) ? 0 : -1;
}

int login(Connection *pConnection)
{
    int check = 0;
    if (pConnection->stage == RECEIVE_COMMAND)
    {
        strcpy(pConnection->user_name, pConnection->train.buf);
        int ret = get_user_salt(pConnection->user_name, pConnection->salt,
                                pConnection->password);
        if (ret < 0)
        {
            return -1;
        }
        if (ret != 0)
        {
            check = 2;// 用户未注册，仍允许此连接重新登录或注册
        }
        if (wire_queue_int(pConnection, check) != sizeof(check))
        {
            return -1;
        }
        if (check != 0)
        {
            return 0;
        }
        train_t train;
        train.length = strlen(pConnection->salt);
        strcpy(train.buf, pConnection->salt);
        if (wire_queue_train(pConnection, &train) !=
           (int)(sizeof(train.length) + train.length))
        {
            return -1;
        }
        pConnection->stage = RECEIVE_LOGIN_PASSWORD;
        return 0;
    }

    const char *crptpasswd = (const char *)&pConnection->train + sizeof(int);
    if (memcmp(pConnection->password, crptpasswd, 49) != 0)
    {
        check = 1;
    }
    memset(pConnection->password, 0, sizeof(pConnection->password));
    pConnection->stage = RECEIVE_COMMAND;
    if (wire_queue_int(pConnection, check) != sizeof(check))
    {
        return -1;
    }
    if (check != 0)
    {
        return 0;
    }

    int user_id;
    if (find_user_id(pConnection->user_name, &user_id) != 0)
    {
        return -1;
    }
    char token[1024] = {0};
    if (make_session_id(pConnection->sid) != 0 || encode(pConnection, token) != 0)
    {
        return -1;
    }
    sysadd(user_id, "login successful!");

    train_t train;
    train.length = strlen(token);
    if (train.length >= (int)sizeof(train.buf))
    {
        return -1;
    }
    strcpy(train.buf, token);
    if (wire_queue_train(pConnection, &train) !=
       (int)(sizeof(train.length) + train.length))
    {
        return -1;
    }
    int lifetime = TOKEN_LIFETIME;
    if (wire_queue_int(pConnection, lifetime) != sizeof(lifetime))
    {
        return -1;
    }
    strcpy(train.buf, "~/");
    train.length = strlen(train.buf);
    if (wire_queue_train(pConnection, &train) !=
       (int)(sizeof(train.length) + train.length))
    {
        return -1;
    }
    pConnection->userId = user_id;
    return 0;
}

int refresh_token(Connection *pConnection)
{
    char token[1024] = {0};
    if (encode(pConnection, token) != 0)
    {
        return -1;
    }
    train_t train;
    train.length = strlen(token);
    if (train.length >= (int)sizeof(train.buf))
    {
        return -1;
    }
    strcpy(train.buf, token);
    int reply = TOKEN_REFRESH_REPLY;
    int lifetime = TOKEN_LIFETIME;
    if (wire_queue_int(pConnection, reply) != sizeof(reply) ||
       wire_queue_train(pConnection, &train) !=
       (int)(sizeof(train.length) + train.length) ||
       wire_queue_int(pConnection, lifetime) != sizeof(lifetime))
    {
        return -1;
    }
    return 0;
}

int token(Connection *pConnection, ClientManager *clients)
{
    train_state_t *pTrain = &pConnection->train;
    int user_id = -1;
    char token[1024] = {0};
    char user_name[100] = {0};
    memcpy(user_name, pTrain->buf, pTrain->pre_length);
    strcpy(token, pTrain->buf + pTrain->pre_length);
    int check = decode(user_name, token, clients, &user_id);
    if (wire_queue_int(pConnection, check) != sizeof(check))
    {
        return -2;
    }
    return check == TOKEN_VALID ? user_id : -1;
}
