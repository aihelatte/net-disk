#include"threadPool.h"
#include "worker.h"

// 路径总长与组件数量分别检查，保留.、..和~的现有语义
static int split_path(char *path, char **words, int capacity)
{
    int count = 0;
    char *save;
    char *part = strtok_r(path, "/", &save);
    while(part != NULL)
    {
        if(count == capacity)
        {
            return -1;
        }
        words[count++] = part;
        part = strtok_r(NULL, "/", &save);
    }
    return count;
}

// 返回上一层时同步更新显示路径，根目录保持~/
static void parent_pwd(char *pwd)
{
    size_t length = strlen(pwd);
    while(length > 2 && pwd[length - 1] == '/')
    {
        pwd[--length] = '\0';
    }
    char *last = strrchr(pwd, '/');
    if(last != NULL && last != pwd + 1)
    {
        *last = '\0';
    }
    else
    {
        strcpy(pwd, "~/");
    }
}

//添加日志
int sysadd(int user_id, char *cmd)
{
    time_t now = time(NULL);
    struct tm tm;
    struct tm *pTm = localtime_r(&now, &tm);
    if(pTm == NULL)
    {
        return -1;
    }
    char user_name[100] = {0};
    if(get_user_name(user_name, user_id) != 0)
    {
        return -1;
    }
    syslog(LOG_INFO,"%4d%2d%2d %2d:%2d:%2d user:%s %s\n",
            pTm->tm_year+1900,pTm->tm_mon+1,pTm->tm_mday, pTm->tm_hour, pTm->tm_min, pTm->tm_sec,
            user_name, cmd);
    return 0;
}
    

int do_cd(char *dir_name, connection_t *pConnection, int user_id)
{
    // 不考虑目标目录地址以/开头，这种情况直接报错
    int check = 0;
    if(dir_name[0] == '/')
    {
        check = 1;
        if(wire_queue_int(pConnection, check) != sizeof(check))
        {
            return -1;
        }
        return 0;
    }
    int code = pConnection->directoryId;
    int parent_code;
    parent_code = code;
    char pwd[sizeof(pConnection->directoryPath)];
    strcpy(pwd, pConnection->directoryPath);
    char *words[20] = {0};
    int i = 0;
    char s[1024] = {0};
    // 如果是..就返回上级目录
    if(strcmp(dir_name, "..") == 0 || strcmp(dir_name, "../") == 0)
    {
        if(code == 0)// 已经在根目录，不能返回上级
        {
            check = 1;
            if(wire_queue_int(pConnection, check) != sizeof(check))
            {
                return -1;
            }
            return 0;
        }
        if(get_parent_id(code, &parent_code) != 0)
        {
            return -1;
        }
        code = parent_code;

        parent_pwd(pwd);
    }
    else if(strcmp(dir_name, "~/") == 0 || strcmp(dir_name, "~") == 0)// 回到根目录
    {
        strcpy(pwd, "~/");
        code = 0;
    }
    // 不是..就去往目的地址
    else
    {
        // 将目标地址切分
        bzero(s, sizeof(s));
        bzero(words, sizeof(words));
        strcpy(s, dir_name);
        i = split_path(s, words, sizeof(words) / sizeof(words[0]));
        if(i <= 0)
        {
            check = 1;
            return wire_queue_int(pConnection, check) == sizeof(check) ? 0 : -1;
        }

        for (int j = 0; j < i; ++j)
        {
            if (strcmp(words[j], ".") == 0)
            {
                continue;
            }
            if (strcmp(words[j], "~") == 0)
            {
                parent_code = code = 0;
                strcpy(pwd, "~/");
                continue;
            }
            if (strcmp(words[j], "..") == 0)
            {
                if (code == 0) // 已经在根目录，不能返回上级
                {
                    check = 1;
                    if(wire_queue_int(pConnection, check) != sizeof(check))
                    {
                        return -1;
                    }
                    return 0;
                }
                if(get_parent_id(code, &parent_code) != 0)
                {
                    return -1;
                }
                code = parent_code;
                parent_pwd(pwd);
                continue;
            }
            // printf("%s\n", words[j]);
            char type[2];
            int ret = get_file_code(user_id, words[j], &code, parent_code, type); // 获取当前文件在虚拟文件目录的id和类型
            if (ret != 0 || strcmp(type, "d") != 0)  // 文件查找失败；不是目录文件；不在可搜索路径下
            {
                check = 1;
                if(wire_queue_int(pConnection, check) != sizeof(check))
                {
                    return -1;
                }
                return 0;
            }
            int separator = strlen(pwd) > 0 && pwd[strlen(pwd) - 1] != '/';
            size_t needed = strlen(pwd) + strlen(words[j]) + separator;
            if(needed >= sizeof(((train_t*)0)->buf))
            {
                check = 1;
                return wire_queue_int(pConnection, check) == sizeof(check) ? 0 : -1;
            }
            if (separator)
            {
                strcat(pwd, "/");
            }
            strcat(pwd, words[j]);
            parent_code = code; // 继续往下遍历
        }
    }
    // printf("pwd = %s, code = %d\n", pwd, code);
    pConnection->directoryId = code;
    strcpy(pConnection->directoryPath, pwd);
    if(wire_queue_int(pConnection, check) != sizeof(check))
    {
        return -1;
    }
    train_t train;
    train.length = strlen(pwd);
    strcpy(train.buf, pwd);
    if(wire_queue_train(pConnection, &train) != (int)sizeof(train.length) + train.length)
    {
        return -1;
    }
    return 0;
}

int do_ls(char *file_name, connection_t *pConnection, int user_id)
{
    // puts("ls");
    // 查看当前目录
    // printf("file_name lth = %ld\n", strlen(file_name));
    int code = pConnection->directoryId;
    int parent_code;
    int check = 0;
    char type[2];
    parent_code = code;
    if(file_name[0] == '/')
    {
        check = 1;
        if(wire_queue_int(pConnection, check) != sizeof(check))
        {
            return -1;
        }
        return 0;
    }
    if(strlen(file_name) == 0 || strcmp(file_name, ".") == 0 || strcmp(file_name, "./") == 0)
    {
        // code不用改变
        printf("local!\n");
    }
    else// 与cd一样的判断
    {
        char *words[20] = {0};
        int i = 0;
        char s[1024] = {0};
        // 是..的情况
        if (strcmp(file_name, "..") == 0 || strcmp(file_name, "../") == 0)
        {
            if (code == 0) // 已经在根目录，不能返回上级
            {
                check = 1;
                if(wire_queue_int(pConnection, check) != sizeof(check))
                {
                    return -1;
                }
                return 0;
            }
            if(get_parent_id(code, &parent_code) != 0)
            {
                return -1;
            }
            code = parent_code;
        }
        // 不是..就去往目的地址
        else
        {
            // 将目标地址切分
            bzero(s, sizeof(s));
            bzero(words, sizeof(words));
            strcpy(s, file_name);
            i = split_path(s, words, sizeof(words) / sizeof(words[0]));
            if(i <= 0)
            {
                check = 1;
                return wire_queue_int(pConnection, check) == sizeof(check) ? 0 : -1;
            }

            for (int j = 0; j < i; ++j)
            {
                if (strcmp(words[j], ".") == 0)
                {
                    continue;
                }
                if (strcmp(words[j], "~") == 0)// 如果以根目录开始，则改变code值为0
                {
                    parent_code = code = 0;
                    continue;
                }
                if (strcmp(words[j], "..") == 0)
                {
                    if (code == 0) // 已经在根目录，不能返回上级
                    {
                        check = 1;
                        if(wire_queue_int(pConnection, check) != sizeof(check))
                        {
                            return -1;
                        }
                        return 0;
                    }
                    if(get_parent_id(code, &parent_code) != 0)
                    {
                        return -1;
                    }
                    code = parent_code;
                    continue;
                }
                // printf("%s\n", words[j]);
                char type[2];
                //一直通过userid和word的配合，用code和parentcode往下遍历
                int ret = get_file_code(user_id, words[j], &code, parent_code, type); // 获取当前文件在虚拟文件目录的id和类型
                if (ret != 0 || strcmp(type, "d") != 0)  // 文件查找失败；不是目录文件；不在可搜索路径下
                {
                    check = 1;
                    if(wire_queue_int(pConnection, check) != sizeof(check))
                    {
                        return -1;
                    }
                    return 0;
                }
                parent_code = code; // 继续往下遍历，遍历结束code就是要浏览的目录的文件id
            }
        }
    }
    printf("code = %d\n", code);
    if(wire_queue_int(pConnection, check) != sizeof(check))
    {
        return -1;
    }
    //code为最后文件夹的code，直接发送他的所有文件
    return find_code_files(pConnection, code, user_id);// 把找到的文件发给客户端

}

int do_pwd(connection_t *pConnection)
{
    // puts("pwd");
    const char *pwd = pConnection->directoryPath;
    printf("pwd : %s\n", pwd);
    train_t train;
    train.length = strlen(pwd);
    strcpy(train.buf, pwd);
    if(wire_queue_train(pConnection, &train) != (int)sizeof(train.length) + train.length)
    {
        return -1;
    }
    return 0;
}

int do_rm(char *file_name, int user_id, connection_t *pConnection)
{
    puts("rm");
    int code = pConnection->directoryId;
    int parent_code;
    char type[2] = {0};
    int check = 0;
    parent_code = code;
    if(file_name[0] == '/' || strcmp(file_name, ".") == 0 || strcmp(file_name, "~/") == 0 || strcmp(file_name, "./") == 0 || strcmp(file_name, "..") == 0 || strcmp(file_name, "../") == 0)
    {
        check = 1;
        if(wire_queue_int(pConnection, check) != sizeof(check))
        {
            return -1;
        }
        return 0;
    }
    else// 不是以上几种情况就遍历获得要删除的文件的id和类型
    {
        // 将目标地址切分
        char *words[20] = {0};
        int i = 0;
        char s[1024] = {0};
        strcpy(s, file_name);
        i = split_path(s, words, sizeof(words) / sizeof(words[0]));
        if(i <= 0)
        {
            check = 1;
            return wire_queue_int(pConnection, check) == sizeof(check) ? 0 : -1;
        }

        if(strcmp(words[i - 1], ".") == 0 || strcmp(words[i - 1], "..") == 0 || strcmp(words[i - 1], "~") == 0)
        {
            check = 1;
            return wire_queue_int(pConnection, check) == sizeof(check) ? 0 : -1;
        }
        for (int j = 0; j < i; ++j)
        {
            if (strcmp(words[j], ".") == 0)
            {
                continue;
            }
            if (strcmp(words[j], "~") == 0)
            {
                parent_code = code = 0;
                continue;
            }
            if (strcmp(words[j], "..") == 0)
            {
                if (code == 0) // 已经在根目录，不能返回上级
                {
                    check = 1;
                    if(wire_queue_int(pConnection, check) != sizeof(check))
                    {
                        return -1;
                    }
                    return 0;
                }
                if(get_parent_id(code, &parent_code) != 0)
                {
                    return -1;
                }
                code = parent_code;
                continue;
            }
            // printf("%s\n", words[j]);
            
            int ret = get_file_code(user_id, words[j], &code, parent_code, type); // 获取当前文件在虚拟文件目录的id和类型
            //printf("ret = %d\n", ret);
            if(ret != 0 || (j != i - 1 && strcmp(type, "d") != 0))// 路径错误
            {
                check = 1;
                if(wire_queue_int(pConnection, check) != sizeof(check))
                {
                    return -1;
                }
                return 0;
            }
            if(j != i-1)
            {
                parent_code = code; // 继续往下遍历，遍历结束code就是要浏览的目录的文件id
            }
        }
    }
    if(code == 0)
    {
        check = 1;
        if(wire_queue_int(pConnection, check) != sizeof(check))
        {
            return -1;
        }
        return 0;
    }
    // printf("code = %d\n", code);
    if(del_virtual_file(code, user_id) != 0)
    {
        return -1;
    }
    if(wire_queue_int(pConnection, check) != sizeof(check))
    {
        return -1;
    }
    return 0;
}

int do_mkdir(char *dir_name, connection_t *pConnection, int user_id)
{
    // puts("mkdir");
    if(!is_valid_file_name(dir_name, strlen(dir_name)))
    {
        int check = 1;
        return wire_queue_int(pConnection, check) == sizeof(check) ? 0 : -1;
    }
    int check = 0;
    int code = pConnection->directoryId;
    //获取当前所在目录的code
    // printf("code = %d\n", code);
    int test;
    char type[2] = {0};
    //看看该文件是否已存在当前目录下
    int ret = get_file_code(user_id, dir_name, &test, code, type);
    if(ret == 0)
    {
        check = 1;
        if(wire_queue_int(pConnection, check) != sizeof(check))
        {
            return -1;
        }
        return 0;
    }
    if(ret < 0)
    {
        return -1;
    }
    check = add_virtual_dir(code, dir_name, user_id);
    if(wire_queue_int(pConnection, check) != sizeof(check))
    {
        return -1;
    }
    return 0;
}

int handle_short_cmd(train_state_t train_get, connection_t *pConnection, int user_id)
{
    sysadd(user_id, train_get.buf);
    char file_name[100] = {0};
    int i = train_get.pre_length;
    while(train_get.buf[i] == ' ')
    {
        ++i;
    }
    if(i > train_get.length || train_get.length - i >= (int)sizeof(file_name))
    {
        return -1;
    }
    strcpy(file_name, train_get.buf + i);

    if (strncmp("cd", train_get.buf, 2) == 0)
    {
        return do_cd(file_name, pConnection, user_id);
    }
    else if (strncmp("ls", train_get.buf, 2) == 0)
    {
        return do_ls(file_name, pConnection, user_id);
    }
    else if (strncmp("pwd", train_get.buf, 3) == 0)
    {
        return do_pwd(pConnection);
    }
    else if (strncmp("rm", train_get.buf, 2) == 0)
    {
        return do_rm(file_name, user_id, pConnection);
    }
    else if (strncmp("mkdir", train_get.buf, 5) == 0)
    {
        return do_mkdir(file_name, pConnection, user_id);
    }
    return -1;
}
