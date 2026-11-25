#include "threadPool.h"
#include "worker.h"

//注册
int register_user(int sockFd)
{
    int check = 0;
    char user_name[100] = {0};
    char passwd[100] = {0};
    train_state_t train_state;
    bzero(&train_state, sizeof(train_state));
    printf("\033[1;34mPlease enter user_name:\033[0m");
    fflush(stdout);
    if(read_input_line(user_name, sizeof(user_name)) <= 0)
    {
        return -1;
    }
    // printf("user_name = %s\n", user_name);
    printf("\033[1;34mPlease enter passwd:\033[0m");
    fflush(stdout);
    if(read_input_line(passwd, sizeof(passwd)) <= 0)
    {
        return -1;
    }
    // 设置小火车，传输状态信息
    train_state.state = REGISTER;
    strcpy(train_state.buf, user_name);
    //printf("train.buf = %s\n", train_state.buf);
    train_state.length = strlen(train_state.buf);
    train_state.pre_length = 0;
    if(wire_send_command(sockFd, &train_state, 0) != (int)(sizeof(train_state.length) + sizeof(train_state.state) + sizeof(train_state.pre_length) + train_state.length))
    {
        return -1;
    }
    
    // 接收盐值发送密文
    char crptpasswd[50] = {0};
    char salt[SATL_LEN + 1] = {0};

    // 接收盐值
    int datalength;
    if(wire_recv_int(sockFd, &datalength) != sizeof(datalength) ||
       datalength != SATL_LEN ||
       recvn(sockFd, salt, datalength) != datalength)
    {
        return -1;
    }

    // 计算出密文并发送
    char *encrypted = crypt(passwd, salt);
    if(encrypted == NULL || encrypted[0] == '*')
    {
        return -1;
    }
    strncpy(crptpasswd, encrypted, sizeof(crptpasswd) - 1);// 保留现有49字节协议，完整散列另行迁移
    train_t train;
    train.length = strlen(crptpasswd);
    strcpy(train.buf, crptpasswd);
    if(wire_send_train(sockFd, &train, MSG_NOSIGNAL) != (int)(sizeof(train.length) + train.length))
    {
        return -1;
    }
    
    if(wire_recv_int(sockFd, &check) != sizeof(check))// 获取注册结果
    {
        return -1;
    }
    return check >= 0 && check <= 2 ? check : -1;
}


int login(int sockFd, char *uName)
{
    int check = 0;
    char user_name[100] = {0};
    // 读取用户名
    printf("\033[1;34mPlease enter user_name:\033[0m");
    fflush(stdout);
    if(read_input_line(user_name, sizeof(user_name)) <= 0)
    {
        return -1;
    }
    train_state_t train_state;
    // 读取密码
    while(1)
    {
        check = 0;
        bzero(&train_state, sizeof(train_state));
        char *passwd = getpass("\033[1;34mPlease enter your password:\033[0m");
        if(passwd == NULL)
        {
            return -1;
        }
        // 设置小火车，传输状态信息和用户名
        train_state.state = LOGIN;
        strcpy(train_state.buf, user_name);
        train_state.length = strlen(train_state.buf);
        train_state.pre_length = 0;
        if(wire_send_command(sockFd, &train_state, 0) != (int)(sizeof(train_state.length) + sizeof(train_state.state) + sizeof(train_state.pre_length) + train_state.length))
        {
            return -1;
        }
        // 先接收看用户是否存在
        if(wire_recv_int(sockFd, &check) != sizeof(check))
        {
            return -1;
        }
        if(check == 2)// 用户未注册
        {
            break;
        }

        if(check != 0)
        {
            return -1;
        }

        // 用户已注册接收盐值
        char crptpasswd[50] = {0};
        char salt[SATL_LEN + 1] = {0};
        int datalength;
        if(wire_recv_int(sockFd, &datalength) != sizeof(datalength) ||
           datalength != SATL_LEN ||
           recvn(sockFd, salt, datalength) != datalength)
        {
            return -1;
        }
        // 计算密文并发送
        char *encrypted = crypt(passwd, salt);
        if(encrypted == NULL || encrypted[0] == '*')
        {
            return -1;
        }
        strncpy(crptpasswd, encrypted, sizeof(crptpasswd) - 1);// 保留现有49字节协议，完整散列另行迁移
        //printf("crptpasswd = %s\n", crptpasswd);
        train_t train;
        bzero(&train, sizeof(train));
        train.length = strlen(crptpasswd);
        strcpy(train.buf, crptpasswd);
        if(wire_send_train(sockFd, &train, MSG_NOSIGNAL) != (int)(sizeof(train.length) + train.length))
        {
            return -1;
        }

        // 接收验证结果
        if(wire_recv_int(sockFd, &check) != sizeof(check))
        {
            return -1;
        }
        if(check != 0 && check != 1)
        {
            return -1;
        }
        //printf("check = %d\n", check);
        if(check == 0)// 密码验证成功
        {
            strcpy(uName, user_name);
            break;
        }
        else
        {
            printf("\033[1;31mPassword is not right!\n\033[0m");
        }
    }
    return check;
}
