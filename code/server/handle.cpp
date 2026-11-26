#include "worker.h"
#include <unordered_map>

// 路径总长与组件数量分别检查，保留.、..和~的现有语义
static int split_path(char *path, char **words, int capacity)
{
    int count = 0;
    char *save;
    char *part = strtok_r(path, "/", &save);
    while (part != NULL)
    {
        if (count == capacity)
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
    while (length > 2 && pwd[length - 1] == '/')
    {
        pwd[--length] = '\0';
    }
    char *last = strrchr(pwd, '/');
    if (last != NULL && last != pwd + 1)
    {
        *last = '\0';
    }
    else
    {
        strcpy(pwd, "~/");
    }
}

//添加日志
int sysadd(int user_id, const char *cmd)
{
    time_t now = time(NULL);
    struct tm tm;
    struct tm *pTm = localtime_r(&now, &tm);
    if (pTm == NULL)
    {
        return -1;
    }
    char user_name[100] = {0};
    if (get_user_name(user_name, user_id) != 0)
    {
        return -1;
    }
    syslog(LOG_INFO,"%4d%2d%2d %2d:%2d:%2d user:%s %s\n",
            pTm->tm_year+1900,pTm->tm_mon+1,pTm->tm_mday, pTm->tm_hour, pTm->tm_min, pTm->tm_sec,
            user_name, cmd);
    return 0;
}


namespace {
// 短命令统一接收参数、控制连接和用户 ID，由具体命令排队回复。
class ShortCommand
{
public:
    virtual ~ShortCommand() = default;
    virtual int execute(const char *arg, Connection *pConnection, int user_id) const = 0;
};

// 解析目标路径，切换当前会话目录并回复新路径。
class CdCommand final : public ShortCommand
{
public:
    int execute(const char *arg, Connection *pConnection, int user_id) const override;
};

// 解析查看路径，按原帧格式返回目录内容。
class LsCommand final : public ShortCommand
{
public:
    int execute(const char *arg, Connection *pConnection, int user_id) const override;
};

// 读取当前会话的路径并排队回复。
class PwdCommand final : public ShortCommand
{
public:
    int execute(const char *arg, Connection *pConnection, int user_id) const override;
};

// 解析当前会话中的目标路径，再按用户 ID 删除。
class RmCommand final : public ShortCommand
{
public:
    int execute(const char *arg, Connection *pConnection, int user_id) const override;
};

// 校验名称与重名状态，在当前会话目录下创建子目录。
class MkdirCommand final : public ShortCommand
{
public:
    int execute(const char *arg, Connection *pConnection, int user_id) const override;
};

using CommandMap = std::unordered_map<int, std::unique_ptr<ShortCommand>>;

// 将每种命令对象交给表持有，后续分发复用同一组对象。
CommandMap buildShortCommands()
{
    CommandMap commands;
    commands.emplace(CD, std::make_unique<CdCommand>());
    commands.emplace(LS, std::make_unique<LsCommand>());
    commands.emplace(PWD, std::make_unique<PwdCommand>());
    commands.emplace(REMOVE, std::make_unique<RmCommand>());
    commands.emplace(MKDIR, std::make_unique<MkdirCommand>());
    return commands;
}

const CommandMap &shortCommands()
{
    static const CommandMap commands = buildShortCommands();
    return commands;
}

// 把状态码排入回复缓冲区。排队失败时返回 -1，由主循环关闭连接。
int queueCommandStatus(Connection *connection, int status)
{
    if (wire_queue_int(connection, status) != sizeof(status))
    {
        return -1;
    }
    return 0;
}

int CdCommand::execute(const char *dir_name, Connection *pConnection, int user_id) const
{
    // 不考虑目标目录地址以/开头，这种情况直接报错
    int check = 0;
    if (dir_name[0] == '/')
    {
        check = 1;
        return queueCommandStatus(pConnection, check);
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
    if (strcmp(dir_name, "..") == 0 || strcmp(dir_name, "../") == 0)
    {
        if (code == 0)// 已经在根目录，不能返回上级
        {
            check = 1;
            return queueCommandStatus(pConnection, check);
        }
        if (get_parent_id(code, &parent_code) != 0)
        {
            return -1;
        }
        code = parent_code;

        parent_pwd(pwd);
    }
    else if (strcmp(dir_name, "~/") == 0 || strcmp(dir_name, "~") == 0)// 回到根目录
    {
        strcpy(pwd, "~/");
        code = 0;
    }
    // 不是..就去往目的地址
    else
    {
        // 将目标地址切分
        strcpy(s, dir_name);
        i = split_path(s, words, sizeof(words) / sizeof(words[0]));
        if (i <= 0)
        {
            check = 1;
            return queueCommandStatus(pConnection, check);
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
                    return queueCommandStatus(pConnection, check);
                }
                if (get_parent_id(code, &parent_code) != 0)
                {
                    return -1;
                }
                code = parent_code;
                parent_pwd(pwd);
                continue;
            }
            char type[2];
            int ret = get_file_code(user_id, words[j], &code, parent_code, type); // 获取当前文件在虚拟文件目录的id和类型
            if (ret != 0 || strcmp(type, "d") != 0)  // 文件查找失败；不是目录文件；不在可搜索路径下
            {
                check = 1;
                return queueCommandStatus(pConnection, check);
            }
            int separator = strlen(pwd) > 0 && pwd[strlen(pwd) - 1] != '/';
            size_t needed = strlen(pwd) + strlen(words[j]) + separator;
            if (needed >= sizeof(((train_t*)0)->buf))
            {
                check = 1;
                return queueCommandStatus(pConnection, check);
            }
            if (separator)
            {
                strcat(pwd, "/");
            }
            strcat(pwd, words[j]);
            parent_code = code; // 继续往下遍历
        }
    }
    pConnection->directoryId = code;
    strcpy(pConnection->directoryPath, pwd);
    if (queueCommandStatus(pConnection, check) != 0)
    {
        return -1;
    }
    train_t train;
    train.length = strlen(pwd);
    strcpy(train.buf, pwd);
    if (wire_queue_train(pConnection, &train) != (int)sizeof(train.length) + train.length)
    {
        return -1;
    }
    return 0;
}

int LsCommand::execute(const char *file_name, Connection *pConnection, int user_id) const
{
    // 查看当前目录
    int code = pConnection->directoryId;
    int parent_code = code;
    int check = 0;
    if (file_name[0] == '/')
    {
        check = 1;
        return queueCommandStatus(pConnection, check);
    }
    if (strlen(file_name) == 0 || strcmp(file_name, ".") == 0 || strcmp(file_name, "./") == 0)
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
                return queueCommandStatus(pConnection, check);
            }
            if (get_parent_id(code, &parent_code) != 0)
            {
                return -1;
            }
            code = parent_code;
        }
        // 不是..就去往目的地址
        else
        {
            // 将目标地址切分
            strcpy(s, file_name);
            i = split_path(s, words, sizeof(words) / sizeof(words[0]));
            if (i <= 0)
            {
                check = 1;
                return queueCommandStatus(pConnection, check);
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
                        return queueCommandStatus(pConnection, check);
                    }
                    if (get_parent_id(code, &parent_code) != 0)
                    {
                        return -1;
                    }
                    code = parent_code;
                    continue;
                }
                char type[2];
                //一直通过userid和word的配合，用code和parentcode往下遍历
                int ret = get_file_code(user_id, words[j], &code, parent_code, type); // 获取当前文件在虚拟文件目录的id和类型
                if (ret != 0 || strcmp(type, "d") != 0)  // 文件查找失败；不是目录文件；不在可搜索路径下
                {
                    check = 1;
                    return queueCommandStatus(pConnection, check);
                }
                parent_code = code; // 继续往下遍历，遍历结束code就是要浏览的目录的文件id
            }
        }
    }
    printf("code = %d\n", code);
    if (queueCommandStatus(pConnection, check) != 0)
    {
        return -1;
    }
    //code为最后文件夹的code，直接发送他的所有文件
    return find_code_files(pConnection, code, user_id);// 把找到的文件发给客户端

}

int PwdCommand::execute(const char *, Connection *pConnection, int) const
{
    const char *pwd = pConnection->directoryPath;
    printf("pwd : %s\n", pwd);
    train_t train;
    train.length = strlen(pwd);
    strcpy(train.buf, pwd);
    if (wire_queue_train(pConnection, &train) != (int)sizeof(train.length) + train.length)
    {
        return -1;
    }
    return 0;
}

int RmCommand::execute(const char *file_name, Connection *pConnection, int user_id) const
{
    puts("rm");
    int code = pConnection->directoryId;
    int parent_code;
    char type[2] = {0};
    int check = 0;
    parent_code = code;
    if (file_name[0] == '/' || strcmp(file_name, ".") == 0 || strcmp(file_name, "~/") == 0 || strcmp(file_name, "./") == 0 || strcmp(file_name, "..") == 0 || strcmp(file_name, "../") == 0)
    {
        check = 1;
        return queueCommandStatus(pConnection, check);
    }
    else// 不是以上几种情况就遍历获得要删除的文件的id和类型
    {
        // 将目标地址切分
        char *words[20] = {0};
        int i = 0;
        char s[1024] = {0};
        strcpy(s, file_name);
        i = split_path(s, words, sizeof(words) / sizeof(words[0]));
        if (i <= 0)
        {
            check = 1;
            return queueCommandStatus(pConnection, check);
        }

        if (strcmp(words[i - 1], ".") == 0 || strcmp(words[i - 1], "..") == 0 || strcmp(words[i - 1], "~") == 0)
        {
            check = 1;
            return queueCommandStatus(pConnection, check);
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
                    return queueCommandStatus(pConnection, check);
                }
                if (get_parent_id(code, &parent_code) != 0)
                {
                    return -1;
                }
                code = parent_code;
                continue;
            }

            int ret = get_file_code(user_id, words[j], &code, parent_code, type); // 获取当前文件在虚拟文件目录的id和类型
            if (ret != 0 || (j != i - 1 && strcmp(type, "d") != 0))// 路径错误
            {
                check = 1;
                return queueCommandStatus(pConnection, check);
            }
            if (j != i-1)
            {
                parent_code = code; // 继续往下遍历，遍历结束code就是要浏览的目录的文件id
            }
        }
    }
    if (code == 0)
    {
        check = 1;
        return queueCommandStatus(pConnection, check);
    }
    if (del_virtual_file(code, user_id) != 0)
    {
        return -1;
    }
    return queueCommandStatus(pConnection, check);
}

int MkdirCommand::execute(const char *dir_name, Connection *pConnection, int user_id) const
{
    if (!is_valid_file_name(dir_name, strlen(dir_name)))
    {
        int check = 1;
        return queueCommandStatus(pConnection, check);
    }
    int check = 0;
    int code = pConnection->directoryId;
    //获取当前所在目录的code
    int test;
    char type[2] = {0};
    //看看该文件是否已存在当前目录下
    int ret = get_file_code(user_id, dir_name, &test, code, type);
    if (ret == 0)
    {
        check = 1;
        return queueCommandStatus(pConnection, check);
    }
    if (ret < 0)
    {
        return -1;
    }
    check = add_virtual_dir(code, dir_name, user_id);
    return queueCommandStatus(pConnection, check);
}

} // namespace

// 从已校验的命令帧提取参数，再按命令编号派发。返回具体命令的结果。
int handle_short_cmd(train_state_t train_get, Connection *pConnection, int user_id)
{
    sysadd(user_id, train_get.buf);
    char file_name[100] = {0};
    int i = train_get.pre_length;
    while (train_get.buf[i] == ' ')
    {
        ++i;
    }
    if (i > train_get.length || train_get.length - i >= (int)sizeof(file_name))
    {
        return -1;
    }
    strcpy(file_name, train_get.buf + i);

    try
    {
        const CommandMap &commands = shortCommands();
        const auto it = commands.find(train_get.state);
        if (it == commands.end())
        {
            return -1;
        }
        // 通过基类接口调用，命令对象决定执行哪一项业务。
        return it->second->execute(file_name, pConnection, user_id);
    }
    catch (const std::bad_alloc &)
    {
        return -1;
    }
}
