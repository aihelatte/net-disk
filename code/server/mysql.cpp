#include "file_descriptor.h"
#include "threadPool.h"
#include "worker.h"

#include <memory>
#include <stdexcept>
#include <vector>

namespace {
// MySQL 的三类句柄由各自的 C API 释放，删除器本身不保存状态。
struct MysqlConnDeleter
{
    void operator()(MYSQL *connection) const
    {
        mysql_close(connection);
    }
};

struct MysqlStmtDeleter
{
    void operator()(MYSQL_STMT *statement) const
    {
        mysql_stmt_close(statement);
    }
};

struct MysqlResDeleter
{
    void operator()(MYSQL_RES *result) const
    {
        mysql_free_result(result);
    }
};

struct DirectoryCloser
{
    void operator()(DIR *directory) const
    {
        closedir(directory);
    }
};

using MysqlConnPtr = std::unique_ptr<MYSQL, MysqlConnDeleter>;
using MysqlStmtPtr = std::unique_ptr<MYSQL_STMT, MysqlStmtDeleter>;
using MysqlResPtr = std::unique_ptr<MYSQL_RES, MysqlResDeleter>;

// 借用仍由 MysqlConnPtr 管理的连接。成功提交前，任何提前返回都会触发回滚。
class Transaction
{
public:
    explicit Transaction(MYSQL *conn) : conn_(conn) {}
    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;

    ~Transaction()
    {
        if (conn_)
        {
            mysql_rollback(conn_);
        }
    }

    int commit()
    {
        if (mysql_commit(conn_) != 0)
        {
            return -1;
        }
        // 提交成功后解除守卫，析构时无需再回滚。
        conn_ = nullptr;
        return 0;
    }
private:
    MYSQL *conn_;
};

int statement_error()
{
    fprintf(stderr, "Database statement failed.\n");
    return -1;
}

//连接数据库
// 按环境配置建立连接。失败时返回空指针，由调用方沿用原错误码。
MysqlConnPtr open_connection()
{
    const char *host = getenv("NET_DISK_DB_HOST");
    const char *user = getenv("NET_DISK_DB_USER");
    const char *passwd = getenv("NET_DISK_DB_PASSWORD");
    const char *db = getenv("NET_DISK_DB_NAME");

    if (host == NULL || host[0] == '\0')
    {
        host = "localhost";
    }
    if (user == NULL || user[0] == '\0')
    {
        user = "net_disk_app";
    }
    if (passwd == NULL || passwd[0] == '\0')
    {
        fprintf(stderr, "NET_DISK_DB_PASSWORD is not set\n");
        return MysqlConnPtr(nullptr);
    }
    if (db == NULL || db[0] == '\0')
    {
        db = "net_disk";
    }

    MysqlConnPtr connection(mysql_init(nullptr));
    if (!connection)
    {
        fprintf(stderr, "mysql_init failed\n");
        return MysqlConnPtr(nullptr);
    }
    if (mysql_real_connect(connection.get(), host, user, passwd, db, 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "Database connection failed.\n");
        return MysqlConnPtr(nullptr);
    }
    return connection;
}

//执行sql语句
int do_query(const char *query, MYSQL *conn)
{
    if (conn == NULL)
    {
        return EXIT_FAILURE;
    }
    int ret = mysql_query(conn, query);
    if (ret != 0)
    {
        fprintf(stderr, "Database query failed.\n");
        return EXIT_FAILURE;
    }
    return 0;
}

// 外部字符串通过参数绑定进入SQL，结果截断或NULL均按失败处理
// 执行一条绑定语句。返回 0 表示成功，1 表示未找到或影响行数不符，-1 表示错误。
int execute_sql(const char *query, const char **values, int count,
                       char **columns, const size_t *capacities, int columnCount,
                       bool requireOneChange = false)
{
    MYSQL_BIND params[5]{};
    MYSQL_BIND outputs[3]{};
    unsigned long lengths[3] = {0};
    my_bool nulls[3] = {0};
    my_bool errors[3] = {0};
    if (count < 0 || count > 5 || columnCount < 0 || columnCount > 3)
    {
        return statement_error();
    }
    MysqlConnPtr conn = open_connection();
    if (!conn || mysql_set_character_set(conn.get(), "utf8") != 0)
    {
        return statement_error();
    }
    MysqlStmtPtr stmt(mysql_stmt_init(conn.get()));
    if (stmt == NULL || mysql_stmt_prepare(stmt.get(), query, strlen(query)) != 0 ||
       mysql_stmt_param_count(stmt.get()) != (unsigned long)count)
    {
        return statement_error();
    }
    for (int i = 0; i < count; ++i)
    {
        if (values[i] == NULL)
        {
            return statement_error();
        }
        params[i].buffer_type = MYSQL_TYPE_STRING;
        params[i].buffer = (void*)values[i];
        params[i].buffer_length = strlen(values[i]);
    }
    if ((count > 0 && mysql_stmt_bind_param(stmt.get(), params) != 0) ||
       mysql_stmt_execute(stmt.get()) != 0)
    {
        return statement_error();
    }
    if (columnCount == 0)
    {
        if (requireOneChange && mysql_stmt_affected_rows(stmt.get()) != 1)
        {
            return 1;
        }
        return 0;
    }
    if (mysql_stmt_field_count(stmt.get()) != (unsigned int)columnCount)
    {
        return statement_error();
    }
    for (int i = 0; i < columnCount; ++i)
    {
        if (columns[i] == NULL || capacities[i] == 0)
        {
            return statement_error();
        }
        outputs[i].buffer_type = MYSQL_TYPE_STRING;
        outputs[i].buffer = columns[i];
        outputs[i].buffer_length = capacities[i] - 1;
        outputs[i].length = &lengths[i];
        outputs[i].is_null = &nulls[i];
        outputs[i].error = &errors[i];
    }
    if (mysql_stmt_bind_result(stmt.get(), outputs) != 0 || mysql_stmt_store_result(stmt.get()) != 0)
    {
        return statement_error();
    }
    int ret = mysql_stmt_fetch(stmt.get());
    if (ret == MYSQL_NO_DATA)
    {
        return 1;
    }
    if (ret != 0)
    {
        return statement_error();
    }
    for (int i = 0; i < columnCount; ++i)
    {
        if (nulls[i] || errors[i] || lengths[i] >= capacities[i])
        {
            return statement_error();
        }
        columns[i][lengths[i]] = '\0';
    }
    return 0;
}

struct DeleteNode
{
    int id;
    char type;
    char md5[MD5_STR_LEN + 1];
};

// 保存查询到的子项，避免目录层级过深时递归耗尽栈空间
int collect_delete_nodes(MYSQL *conn, MYSQL_RES *result,
                                std::vector<DeleteNode> &nodes)
{
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result)) != NULL)
    {
        if (row[0] == NULL || row[1] == NULL ||
           (strcmp(row[1], "f") != 0 && strcmp(row[1], "d") != 0))
        {
            return -1;
        }
        char *end;
        errno = 0;
        long id = strtol(row[0], &end, 10);
        if (errno != 0 || end == row[0] || *end != '\0' || id <= 0 || id > INT_MAX)
        {
            return -1;
        }
        if (row[1][0] == 'f' && (row[2] == NULL || strlen(row[2]) != MD5_STR_LEN ||
           strspn(row[2], "0123456789abcdefABCDEF") != MD5_STR_LEN))
        {
            return -1;
        }
        // parent_id 异常成环时停止删除，避免重复处理同一记录
        for (const DeleteNode &node : nodes)
        {
            if (node.id == id)
            {
                return -1;
            }
        }
        DeleteNode node{static_cast<int>(id), row[1][0], {}};
        if (node.type == 'f')
        {
            strcpy(node.md5, row[2]);
        }
        try
        {
            nodes.push_back(node);
        }
        catch (const std::bad_alloc &)
        {
            return -1;
        }
        catch (const std::length_error &)
        {
            return -1;
        }
    }
    return mysql_errno(conn) == 0 ? 0 : -1;
}

// 查询当前删除目标或它的子项。参数 requireOne 仅用于要求根目标恰好存在一条。
int select_delete_nodes(MYSQL *conn, const char *query, size_t queryCapacity,
                        int length, bool requireOne, std::vector<DeleteNode> &nodes)
{
    if (length < 0 || static_cast<size_t>(length) >= queryCapacity)
    {
        return -1;
    }
    if (mysql_query(conn, query) != 0)
    {
        return -1;
    }
    MysqlResPtr result(mysql_store_result(conn));
    if (!result || (requireOne && mysql_num_rows(result.get()) != 1))
    {
        return -1;
    }
    return collect_delete_nodes(conn, result.get(), nodes);
}

} // namespace

// 注册信息使用绑定参数，避免将用户名和密文拼进SQL
int add_user(char *user_name, char *salt, char *crptpasswd)
{
    const char *values[] = {user_name, salt, crptpasswd};
    return execute_sql("insert into client_info(username, salt, cryptpasswd, pwd, code) values(?, ?, ?, '~/', 0)",
                       values, 3, NULL, NULL, 0);
}

// 根据用户名获取盐值和密文
int get_user_salt(char *user_name, char *salt, char *crptpasswd)
{
    MysqlConnPtr connection = open_connection();
    MYSQL *conn = connection.get();
    if (!conn)
    {
        return -1;
    }
    int ret = mysql_query(conn, "set names 'utf8'");
    if (ret != 0)
    {
        fprintf(stderr, "Database query failed.\n");
        return -1;
    }
    ret = do_query("select username, salt, cryptpasswd from client_info", conn);
    if (ret != 0)
    {
        return -1;
    }
    MysqlResPtr result(mysql_use_result(conn));
    if (result == NULL)
    {
        fprintf(stderr, "Database query failed.\n");
        return -1;
    }
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result.get())) != NULL)
    {
        if (row[0] == NULL || row[1] == NULL || row[2] == NULL)
        {
            return -1;
        }
        if (strcmp(row[0], user_name) == 0)
        {
            if (strlen(row[1]) >= SATL_LEN + 1)
            {
                return -1;
            }
            strcpy(salt, row[1]);
            if (strlen(row[2]) >= 50)
            {
                return -1;
            }
            strcpy(crptpasswd, row[2]);
            return 0;
        }
    }
    if (mysql_errno(conn) != 0)
    {
        return -1;
    }
    return 1;
}

// 根据用户名获取用户id
int find_user_id(char *user_name, int *puser_id)
{
    MysqlConnPtr connection = open_connection();
    MYSQL *conn = connection.get();
    if (!conn)
    {
        return -1;
    }
    int ret = mysql_query(conn, "set names 'utf8'");
    if (ret != 0)
    {
        fprintf(stderr, "Database query failed.\n");
        return -1;
    }
    ret = do_query("select username, id from client_info", conn);
    if (ret != 0)
    {
        return -1;
    }
    MysqlResPtr result(mysql_use_result(conn));
    if (result == NULL)
    {
        fprintf(stderr, "Database query failed.\n");
        return -1;
    }
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result.get())) != NULL)
    {
        if (row[0] == NULL || row[1] == NULL)
        {
            return -1;
        }
        if (strcmp(row[0], user_name) == 0)
        {
            char *end;
            errno = 0;
            long value = strtol(row[1], &end, 10);
            if (errno != 0 || end == row[1] || *end != '\0' || value < 0 || value > INT_MAX)
            {
                return -1;
            }
            *puser_id = value;

            return 0;
        }
    }
    if (mysql_errno(conn) != 0)
    {
        return -1;
    }
    return 1;
}

// 根据用户id获取用户名
int get_user_name(char *user_name, const int user_id)
{
    MysqlConnPtr connection = open_connection();
    MYSQL *conn = connection.get();
    if (!conn)
    {
        return -1;
    }
    int ret = mysql_query(conn, "set names 'utf8'");
    if (ret != 0)
    {
        fprintf(stderr, "Database query failed.\n");
        return -1;
    }
    ret = do_query("select username, id from client_info", conn);
    if (ret != 0)
    {
        return -1;
    }
    MysqlResPtr result(mysql_use_result(conn));
    if (result == NULL)
    {
        fprintf(stderr, "Database query failed.\n");
        return -1;
    }
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result.get())) != NULL)
    {
        if (row[1] == NULL || row[0] == NULL)
        {
            return -1;
        }
        if (user_id == atoi(row[1]))     //如果userid符合，直接拷贝并返回
        {
            if (strlen(row[0]) >= PROTOCOL_USER_NAME_CAPACITY)
            {
                return -1;
            }
            strcpy(user_name, row[0]);

            return 0;
        }
    }
    if (mysql_errno(conn) != 0)
    {
        return -1;
    }
    return 1;
}

// 初始化本地文件表
int init_local_file()
{
    std::unique_ptr<DIR, DirectoryCloser> dirp(opendir("NetDisk"));
    if (dirp == NULL)
    {
        return -1;
    }
    int status = -1;
    struct dirent *pdirent;
    while (1)
    {
        errno = 0;
        pdirent = readdir(dirp.get());
        if (pdirent == NULL)
        {
            status = errno == 0 ? 0 : -1;
            break;
        }
        if (pdirent->d_name[0] == '.')
        {
            continue;
        }
        const char *values[] = {pdirent->d_name};
        char md5_str[MD5_STR_LEN + 1] = {0};
        char *columns[] = {md5_str};
        size_t capacities[] = {sizeof(md5_str)};
        int ret = execute_sql("select md5 from local_files where real_file_name = ?",
                              values, 1, columns, capacities, 1);
        if (ret < 0)
        {
            break;
        }
        if (ret == 0)
        {
            continue;
        }
        if (Compute_file_md5(pdirent->d_name, md5_str) != 0)
        {
            break;
        }
        // 已有相同内容时保留未登记的物理文件，不插入重复 MD5
        ret = if_local_exist(md5_str);
        if (ret < 0)
        {
            break;
        }
        if (ret == 0)
        {
            continue;
        }
        const char *insert[] = {md5_str, pdirent->d_name, md5_str};
        if (execute_sql("insert into local_files(md5, link_num, real_file_name) "
                       "select ?, count(*), ? from virtual_file where md5 = ? and type = 'f'",
                       insert, 3, NULL, NULL, 0) != 0)
        {
            break;
        }
    }
    return status;
}

// 查找本地是否已经有文件存在
int if_local_exist(char *md5_str)
{
    const char *values[] = {md5_str};
    char name[1024];
    char *columns[] = {name};
    size_t capacities[] = {sizeof(name)};
    return execute_sql("select real_file_name from local_files where md5 = ?",
                       values, 1, columns, capacities, 1);
}

// 查询本地文件是否已经接收完成
int get_local_file_status(char *md5_str, char *real_name, int *complete)
{
    const char *values[] = {md5_str, real_name};
    char status[16];
    char *columns[] = {status};
    size_t capacities[] = {sizeof(status)};
    int ret = execute_sql("select upload_complete from local_files where md5 = ? and real_file_name = ?",
                          values, 2, columns, capacities, 1);
    if (ret != 0)
    {
        return ret;
    }
    if (strcmp(status, "0") == 0)
    {
        *complete = 0;
    }
    else if (strcmp(status, "1") == 0)
    {
        *complete = 1;
    }
    else
    {
        return -1;
    }
    return 0;
}

// 更新后检查记录仍存在，原状态相同时也允许成功
int set_local_file_status(char *md5_str, char *real_name, int complete)
{
    if (complete != 0 && complete != 1)
    {
        return -1;
    }
    const char *values[] = {complete == 0 ? "0" : "1", md5_str, real_name};
    if (execute_sql("update local_files set upload_complete = ? where md5 = ? and real_file_name = ?",
                   values, 3, NULL, NULL, 0) != 0)
    {
        return -1;
    }
    int actual;
    if (get_local_file_status(md5_str, real_name, &actual) != 0 || actual != complete)
    {
        return -1;
    }
    return 0;
}

// 增加本地文件的链接数
int add_link(char *md5_str)
{
    MYSQL_BIND params[2];
    MYSQL_BIND output;
    my_bool isNull = 0;
    int localId = 0;
    int maxLink = INT_MAX;
    const char *query = "select id from local_files where md5 = ? for update";
    if (!md5_str)
    {
        return statement_error();
    }
    MysqlConnPtr connection = open_connection();
    MYSQL *conn = connection.get();
    if (!conn)
    {
        return statement_error();
    }
    if (mysql_set_character_set(conn, "utf8") != 0)
    {
        return statement_error();
    }
    if (mysql_autocommit(conn, 0) != 0)
    {
        return statement_error();
    }
    Transaction transaction(conn);
    MysqlStmtPtr stmt(mysql_stmt_init(conn));
    if (stmt == NULL || mysql_stmt_prepare(stmt.get(), query, strlen(query)) != 0)
    {
        return statement_error();
    }
    bzero(params, sizeof(params));
    params[0].buffer_type = MYSQL_TYPE_STRING;
    params[0].buffer = md5_str;
    params[0].buffer_length = strlen(md5_str);
    bzero(&output, sizeof(output));
    output.buffer_type = MYSQL_TYPE_LONG;
    output.buffer = &localId;
    output.buffer_length = sizeof(localId);
    output.is_null = &isNull;
    // 锁定并确认恰好一条记录，避免重复 MD5 时修改错误记录
    if (mysql_stmt_bind_param(stmt.get(), params) != 0 || mysql_stmt_execute(stmt.get()) != 0 ||
       mysql_stmt_bind_result(stmt.get(), &output) != 0 || mysql_stmt_store_result(stmt.get()) != 0 ||
       mysql_stmt_num_rows(stmt.get()) != 1 || mysql_stmt_fetch(stmt.get()) != 0 ||
       isNull || localId <= 0)
    {
        return statement_error();
    }
    // 显式关闭并检查结果，因为此处要根据关闭状态决定是否继续事务。
    if (mysql_stmt_close(stmt.release()) != 0)
    {
        return statement_error();
    }

    // 在数据库内直接加一，避免使用并发操作前读取的旧值
    query = "update local_files set link_num = link_num + 1 "
            "where id = ? and link_num >= 0 and link_num < ?";
    stmt.reset(mysql_stmt_init(conn));
    if (stmt == NULL || mysql_stmt_prepare(stmt.get(), query, strlen(query)) != 0)
    {
        return statement_error();
    }
    bzero(params, sizeof(params));
    params[0].buffer_type = MYSQL_TYPE_LONG;
    params[0].buffer = &localId;
    params[0].buffer_length = sizeof(localId);
    params[1].buffer_type = MYSQL_TYPE_LONG;
    params[1].buffer = &maxLink;
    params[1].buffer_length = sizeof(maxLink);
    if (mysql_stmt_bind_param(stmt.get(), params) != 0 || mysql_stmt_execute(stmt.get()) != 0 ||
       mysql_stmt_affected_rows(stmt.get()) != 1)
    {
        return statement_error();
    }
    if (mysql_stmt_close(stmt.release()) != 0)
    {
        return statement_error();
    }
    if (transaction.commit() != 0)
    {
        return statement_error();
    }
    return 0;
}

// 减少本地文件的链接数
int minus_link(char *md5_str)
{
    const char *values[] = {md5_str};
    char number[32];
    char *columns[] = {number};
    size_t capacities[] = {sizeof(number)};
    if (execute_sql("select link_num from local_files where md5 = ?",
                   values, 1, columns, capacities, 1) != 0)
    {
        return -1;
    }
    char *end;
    errno = 0;
    long link_num = strtol(number, &end, 10);
    if (errno != 0 || end == number || *end != '\0' || link_num < 0 ||
       link_num > INT_MAX || link_num == 0)
    {
        return -1;
    }
    --link_num;
    snprintf(number, sizeof(number), "%ld", link_num);
    const char *updates[] = {number, md5_str};
    return execute_sql("update local_files set link_num = ? where md5 = ?",
                       updates, 2, NULL, NULL, 0);
}

// 在本地文件表中新增内容
int add_local_file(char *md5_str, char *real_name)
{
    const char *values[] = {md5_str, real_name};
    return execute_sql("insert into local_files(md5, link_num, real_file_name) values(?, 1, ?)",
                       values, 2, NULL, NULL, 0);
}

// 在调用方持有的连接上执行绑定语句，使多个操作属于同一事务
// 在调用方的事务连接上执行一条语句，成功时写入行数并返回 0。
static int upload_statement(MYSQL *conn, const char *query, const char **values, int count,
                            char **columns, const size_t *capacities, int columnCount,
                            my_ulonglong *rows)
{
    MYSQL_BIND params[5], outputs[3];
    unsigned long lengths[3] = {0};
    my_bool nulls[3] = {0}, errors[3] = {0};
    int ignoredId = 0;
    if (count < 0 || count > 5 || columnCount < 0 || columnCount > 3)
    {
        return -1;
    }
    MysqlStmtPtr stmt(mysql_stmt_init(conn));
    if (stmt == NULL || mysql_stmt_prepare(stmt.get(), query, strlen(query)) != 0 ||
       mysql_stmt_param_count(stmt.get()) != (unsigned long)count)
    {
        return -1;
    }
    bzero(params, sizeof(params));
    for (int i = 0; i < count; ++i)
    {
        if (values[i] == NULL)
        {
            return -1;
        }
        params[i].buffer_type = MYSQL_TYPE_STRING;
        params[i].buffer = (void *)values[i];
        params[i].buffer_length = strlen(values[i]);
    }
    if ((count > 0 && mysql_stmt_bind_param(stmt.get(), params) != 0) ||
       mysql_stmt_execute(stmt.get()) != 0)
    {
        return -1;
    }
    if (mysql_stmt_field_count(stmt.get()) == 0)
    {
        *rows = mysql_stmt_affected_rows(stmt.get());
        // 显式关闭以保留关闭失败的返回值，release 避免析构时再关闭一次。
        if (mysql_stmt_close(stmt.release()) != 0)
        {
            return -1;
        }
        return 0;
    }
    if ((columnCount == 0 && mysql_stmt_field_count(stmt.get()) != 1) ||
       (columnCount > 0 && mysql_stmt_field_count(stmt.get()) != (unsigned int)columnCount))
    {
        return -1;
    }
    bzero(outputs, sizeof(outputs));
    if (columnCount == 0)
    {
        // 仅统计记录数的查询都返回 id，也先绑定结果以兼容旧客户端库
        outputs[0].buffer_type = MYSQL_TYPE_LONG;
        outputs[0].buffer = &ignoredId;
        outputs[0].buffer_length = sizeof(ignoredId);
    }
    for (int i = 0; i < columnCount; ++i)
    {
        if (columns[i] == NULL || capacities[i] == 0)
        {
            return -1;
        }
        outputs[i].buffer_type = MYSQL_TYPE_STRING;
        outputs[i].buffer = columns[i];
        outputs[i].buffer_length = capacities[i] - 1;
        outputs[i].length = &lengths[i];
        outputs[i].is_null = &nulls[i];
        outputs[i].error = &errors[i];
    }
    if (mysql_stmt_bind_result(stmt.get(), outputs) != 0 ||
       mysql_stmt_store_result(stmt.get()) != 0)
    {
        return -1;
    }
    *rows = mysql_stmt_num_rows(stmt.get());
    if (*rows == 1 && columnCount > 0)
    {
        if (mysql_stmt_fetch(stmt.get()) != 0)
        {
            return -1;
        }
        for (int i = 0; i < columnCount; ++i)
        {
            if (nulls[i] || errors[i] || lengths[i] >= capacities[i])
            {
                return -1;
            }
            columns[i][lengths[i]] = '\0';
        }
    }
    if (mysql_stmt_close(stmt.release()) != 0)
    {
        return -1;
    }
    return 0;
}

// 根据目录、文件名、MD5 和大小准备上传。成功返回 0 并交出持锁 fd。
// 元数据冲突返回 1，其余失败返回 -1。网络传输从事务提交后开始。
int prepare_upload(int parent_id, char *name, int owner_id, char *md5_str,
                   int64_t fileSize, char *real_name, int *pfd, int *complete)
{
    int oldFile = 0, localExists = 0, localComplete = 0;
    my_ulonglong rows;
    char owner[32], parent[32], size[32], oldMd5[MD5_STR_LEN + 1];
    char oldSize[32], oldType[2], localStatus[16], localLinks[32];
    char *virtualColumns[] = {oldMd5, oldSize, oldType};
    size_t virtualCapacities[] = {sizeof(oldMd5), sizeof(oldSize), sizeof(oldType)};
    char *localColumns[] = {real_name, localStatus};
    size_t localCapacities[] = {1024, sizeof(localStatus)};
    const char *userValues[] = {owner};
    const char *virtualValues[] = {owner, parent, name};
    const char *md5Values[] = {md5_str};
    const char *sizeValues[] = {md5_str, size};
    *pfd = -1;
    *complete = 0;
    if (parent_id < 0 || owner_id < 0 || fileSize < 0 ||
       !is_valid_file_name(name, strlen(name)) || strlen(name) > 255 ||
       strlen(md5_str) != MD5_STR_LEN ||
       strspn(md5_str, "0123456789abcdefABCDEF") != MD5_STR_LEN)
    {
        return -1;
    }
    snprintf(owner, sizeof(owner), "%d", owner_id);
    snprintf(parent, sizeof(parent), "%d", parent_id);
    snprintf(size, sizeof(size), "%lld", (long long)fileSize);
    FileDescriptor fd;
    MysqlConnPtr connection = open_connection();
    MYSQL *conn = connection.get();
    if (!conn)
    {
        return -1;
    }
    if (mysql_set_character_set(conn, "utf8") != 0)
    {
        return -1;
    }
    if (mysql_autocommit(conn, 0) != 0)
    {
        return -1;
    }
    Transaction transaction(conn);
    if (upload_statement(conn, "select id from client_info where id = ? for update",
                        userValues, 1, NULL, NULL, 0, &rows) != 0 || rows != 1)
    {
        return -1;
    }
    if (parent_id != 0)
    {
        const char *parentValues[] = {parent, owner};
        if (upload_statement(conn, "select id from virtual_file where id = ? and owner_id = ? and type = 'd' for update",
                            parentValues, 2, NULL, NULL, 0, &rows) != 0 || rows != 1)
        {
            return -1;
        }
    }
    if (upload_statement(conn, "select md5, filesize, type from virtual_file where owner_id = ? and parent_id = ? and filename = ? for update",
                        virtualValues, 3, virtualColumns, virtualCapacities, 3, &rows) != 0 || rows > 1)
    {
        return -1;
    }
    if (rows == 1)
    {
        char *end;
        errno = 0;
        long long oldValue = strtoll(oldSize, &end, 10);
        if (errno != 0 || end == oldSize || *end != '\0' || oldValue < 0 ||
           (int64_t)oldValue != oldValue)
        {
            return -1;
        }
        if (strcmp(oldType, "f") != 0 || strcmp(oldMd5, md5_str) != 0 || oldValue != fileSize)
        {
            return 1;
        }
        oldFile = 1;
    }
    // 同一物理内容的已登记大小不同，不能用本次大小改写共享文件状态
    if (upload_statement(conn, "select id from virtual_file where md5 = ? and type = 'f' and (filesize is null or filesize <> CAST(? AS SIGNED)) for update",
                        sizeValues, 2, NULL, NULL, 0, &rows) != 0)
    {
        return -1;
    }
    if (rows != 0)
    {
        return 1;
    }
    if (upload_statement(conn, "select real_file_name, upload_complete from local_files where md5 = ? for update",
                        md5Values, 1, localColumns, localCapacities, 2, &rows) != 0 || rows > 1)
    {
        return -1;
    }
    localExists = rows == 1;
    if (localExists)
    {
        if (strcmp(localStatus, "0") == 0)
        {
            localComplete = 0;
        }
        else if (strcmp(localStatus, "1") == 0)
        {
            localComplete = 1;
        }
        else
        {
            return -1;
        }
        fd.reset(open_in_root("NetDisk", real_name, O_RDWR, 0));
        if (fd.get() == -1 && errno == ENOENT)
        {
            fd.reset(open_in_root("NetDisk", real_name, O_RDWR | O_CREAT | O_EXCL, 0666));
            localComplete = 0;
        }
        if (fd.get() == -1 || flock(fd.get(), LOCK_EX | LOCK_NB) == -1)
        {
            return -1;
        }
        struct stat statbuf;
        if (fstat(fd.get(), &statbuf) == -1)
        {
            return -1;
        }
        if (localComplete == 1 && statbuf.st_size != fileSize)
        {
            localComplete = 0;
        }
        if (localComplete == 0)
        {
            const char *stateValues[] = {md5_str, real_name};
            if (upload_statement(conn, "update local_files set upload_complete = 0 where md5 = ? and real_file_name = ?",
                                stateValues, 2, NULL, NULL, 0, &rows) != 0)
            {
                return -1;
            }
        }
    }
    else
    {
        // 历史记录可能已有虚拟引用但缺少本地记录，先数出已有引用
        if (upload_statement(conn, "select id from virtual_file where md5 = ? and type = 'f' for update",
                            md5Values, 1, NULL, NULL, 0, &rows) != 0 ||
           rows > (my_ulonglong)INT_MAX - (oldFile ? 0 : 1))
        {
            return -1;
        }
        snprintf(localLinks, sizeof(localLinks), "%llu", (unsigned long long)rows + (oldFile ? 0 : 1));
        // 只为新内容分配物理名称，已登记的 MD5 始终使用原映射。
        if (upload_statement(conn, "select concat('blob-', UUID())", NULL, 0,
                            localColumns, localCapacities, 1, &rows) != 0 || rows != 1 ||
           !is_valid_file_name(real_name, strlen(real_name)))
        {
            return -1;
        }
        const char *localValues[] = {md5_str, localLinks, real_name};
        if (upload_statement(conn, "insert into local_files(md5, link_num, real_file_name, upload_complete) values(?, ?, ?, 0)",
                            localValues, 3, NULL, NULL, 0, &rows) != 0 || rows != 1)
        {
            return -1;
        }
        fd.reset(open_in_root("NetDisk", real_name, O_RDWR | O_CREAT | O_EXCL, 0666));
        if (fd.get() == -1 || flock(fd.get(), LOCK_EX | LOCK_NB) == -1)
        {
            return -1;
        }
    }
    if (!oldFile)
    {
        const char *newValues[] = {parent, name, owner, md5_str, size};
        if (upload_statement(conn, "insert into virtual_file(parent_id, filename, owner_id, md5, filesize, type) values(?, ?, ?, ?, ?, 'f')",
                            newValues, 5, NULL, NULL, 0, &rows) != 0 || rows != 1)
        {
            return -1;
        }
        if (localExists && (upload_statement(conn, "update local_files set link_num = link_num + 1 where md5 = ? and link_num >= 0 and link_num < 2147483647",
                                            md5Values, 1, NULL, NULL, 0, &rows) != 0 || rows != 1))
        {
            return -1;
        }
    }
    if (transaction.commit() != 0)
    {
        return -1;
    }
    // 事务已提交，把持锁的 fd 交给传输代码，后续写入仍使用同一把文件锁。
    *pfd = fd.release();
    *complete = localComplete;
    return 0;
}

// 获取文件本地真实名称
int get_real_file_name(char *md5_str, char *real_file_name)
{
    const char *values[] = {md5_str};
    char *columns[] = {real_file_name};
    size_t capacities[] = {1024};
    return execute_sql("select real_file_name from local_files where md5 = ?",
                       values, 1, columns, capacities, 1);
}

// 根据文件名和用户id获取文件在虚拟文件系统中的id，parent_id, 类型
int get_file_code(int user_id, const char *filename, int *pcode, int parent_id, char *type)
{
    char owner[32], parent[32], id[32];
    snprintf(owner, sizeof(owner), "%d", user_id);
    snprintf(parent, sizeof(parent), "%d", parent_id);
    const char *values[] = {owner, filename, parent};
    char *columns[] = {id, type};
    size_t capacities[] = {sizeof(id), 2};
    int ret = execute_sql("select id, type from virtual_file where owner_id = ? and filename = ? and parent_id = ?",
                          values, 3, columns, capacities, 2);
    if (ret != 0)
    {
        return ret;
    }
    char *end;
    errno = 0;
    long code = strtol(id, &end, 10);
    if (errno != 0 || end == id || *end != '\0' || code < 0 || code > INT_MAX)
    {
        return -1;
    }
    *pcode = code;
    return 0;
}

// 增加虚拟文件夹
int add_virtual_dir(int parent_id, const char *filename, int owner_id)
{
    char parent[32], owner[32];
    snprintf(parent, sizeof(parent), "%d", parent_id);
    snprintf(owner, sizeof(owner), "%d", owner_id);
    // 另一个会话可能已经删除当前目录；只有父目录仍存在时才能创建子项。
    if (parent_id != 0)
    {
        const char *values[] = {filename, parent, owner};
        return execute_sql("insert into virtual_file(parent_id, filename, owner_id, md5, filesize, type) "
                           "select id, ?, owner_id, '0', 0, 'd' from virtual_file "
                           "where id = ? and owner_id = ? and type = 'd'",
                           values, 3, NULL, NULL, 0, true);
    }
    const char *values[] = {parent, filename, owner};
    return execute_sql("insert into virtual_file(parent_id, filename, owner_id, md5, filesize, type) values(?, ?, ?, '0', 0, 'd')",
                       values, 3, NULL, NULL, 0);
}

// 增加虚拟文件
int add_virtual_file(int parent_id, char *filename, int owner_id, char *md5_str, int64_t filesize)
{
    char parent[32], owner[32], size[32];
    snprintf(parent, sizeof(parent), "%d", parent_id);
    snprintf(owner, sizeof(owner), "%d", owner_id);
    snprintf(size, sizeof(size), "%lld", (long long)filesize);
    const char *values[] = {parent, filename, owner, md5_str, size};
    return execute_sql("insert into virtual_file(parent_id, filename, owner_id, md5, filesize, type) values(?, ?, ?, ?, ?, 'f')",
                       values, 5, NULL, NULL, 0);
}

// ls功能的实现，把文件id为code的文件夹下的文件全部发给客户端
int find_code_files(Connection *pConnection, int code, int user_id)
{
    MysqlConnPtr connection = open_connection();
    MYSQL *conn = connection.get();
    if (!conn)
    {
        return -1;
    }
    int ret = mysql_query(conn, "set names 'utf8'");
    if (ret != 0)
    {
        fprintf(stderr, "Database query failed.\n");
        return -1;
    }
    char query[1024] = {0};
    //可以通过parent_id找到目录下的所有文件
    ret = snprintf(query, sizeof(query),
                   "select v.filename from virtual_file v where v.parent_id = %d and v.owner_id = %d "
                   "and (v.type = 'd' or (v.type = 'f' and exists "
                   "(select 1 from local_files l where l.md5 = v.md5 and l.upload_complete = 1)))",
                   code, user_id);
    if (ret < 0 || ret >= (int)sizeof(query))
    {
        return -1;
    }
    ret = do_query(query, conn);
    if (ret != 0)
    {
        return -1;
    }
    MysqlResPtr result(mysql_use_result(conn));
    if (result == NULL)
    {
        fprintf(stderr, "Database query failed.\n");
        return -1;
    }
    MYSQL_ROW row;
    train_t train;
    while ((row = mysql_fetch_row(result.get())) != NULL)
    {
        if (row[0] == NULL)
        {
            return -1;
        }
        train.length = strlen(row[0]);
        bzero(train.buf, sizeof(train.buf));
        if (strlen(row[0]) >= sizeof(train.buf))
        {
            return -1;
        }
        strcpy(train.buf, row[0]);
        //每找到一个文件，就发给客户端
        if (wire_queue_train(pConnection, &train) != (int)sizeof(train.length) + train.length)
        {
            return -1;
        }
    }
    if (mysql_errno(conn) != 0)
    {
        return -1;
    }
    // 发送长度0表示结束
    train.length = 0;
    if (wire_queue_train(pConnection, &train) != (int)sizeof(train.length) + train.length)
    {
        return -1;
    }
    return 0;
}

// 获取文件md5值，也可以检查文件是否存在，存在返回0，不存在返回1
int get_virtual_file_md5(char *file, int user_id, int parent_id, char *md5_str)
{
    char owner[32], parent[32];
    snprintf(owner, sizeof(owner), "%d", user_id);
    snprintf(parent, sizeof(parent), "%d", parent_id);
    const char *values[] = {owner, file, parent};
    char *columns[] = {md5_str};
    size_t capacities[] = {MD5_STR_LEN + 1};
    return execute_sql("select md5 from virtual_file where owner_id = ? and filename = ? and parent_id = ?",
                       values, 3, columns, capacities, 1);
}

// 获取虚拟文件登记的完整大小
int get_virtual_file_size(char *file, int user_id, int parent_id, int64_t *size)
{
    char owner[32], parent[32], number[32];
    snprintf(owner, sizeof(owner), "%d", user_id);
    snprintf(parent, sizeof(parent), "%d", parent_id);
    const char *values[] = {owner, file, parent};
    char *columns[] = {number};
    size_t capacities[] = {sizeof(number)};
    int ret = execute_sql("select filesize from virtual_file where owner_id = ? and filename = ? and parent_id = ? and type = 'f'",
                          values, 3, columns, capacities, 1);
    if (ret != 0)
    {
        return ret;
    }
    char *end;
    errno = 0;
    long long value = strtoll(number, &end, 10);
    if (errno != 0 || end == number || *end != '\0' || value < 0 ||
       (int64_t)value != value)
    {
        return -1;
    }
    *size = (int64_t)value;
    return 0;
}

// 按用户 ID 删除目标及其后代，并在同一事务内更新引用计数。
// 所有更新成功且提交后返回 0，任一环节失败返回 -1。
int del_virtual_file(int code, int user_id)
{
    if (code <= 0 || user_id < 0)
    {
        return -1;
    }
    MysqlConnPtr connection = open_connection();
    MYSQL *conn = connection.get();
    if (!conn)
    {
        return -1;
    }
    if (mysql_query(conn, "set names 'utf8'") != 0)
    {
        return -1;
    }
    if (mysql_autocommit(conn, 0) != 0)
    {
        return -1;
    }
    Transaction transaction(conn);
    std::vector<DeleteNode> nodes;
    char query[1024];

    int ret = snprintf(query, sizeof(query),
                       "select id, type, md5 from virtual_file where id = %d and owner_id = %d for update",
                       code, user_id);
    if (select_delete_nodes(conn, query, sizeof(query), ret, true, nodes) != 0)
    {
        return -1;
    }

    // 逐层收集，子项总在父项之后，反向遍历即可从内向外删除
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        ret = snprintf(query, sizeof(query),
                       "select id, type, md5 from virtual_file where parent_id = %d and owner_id = %d for update",
                       nodes[i].id, user_id);
        if (select_delete_nodes(conn, query, sizeof(query), ret, false, nodes) != 0)
        {
            return -1;
        }
    }
    for (size_t i = nodes.size(); i > 0; --i)
    {
        const DeleteNode &node = nodes[i - 1];
        if (node.type == 'f')
        {
            // MD5 已校验为固定长度的十六进制字符，不含 SQL 特殊字符
            ret = snprintf(query, sizeof(query),
                           "select id, link_num from local_files where md5 = '%s' for update", node.md5);
            if (ret < 0 || ret >= (int)sizeof(query))
            {
                return -1;
            }
            if (mysql_query(conn, query) != 0)
            {
                return -1;
            }
            MysqlResPtr result(mysql_store_result(conn));
            if (!result || mysql_num_rows(result.get()) > 1)
            {
                return -1;
            }
            if (mysql_num_rows(result.get()) == 1)
            {
                MYSQL_ROW row = mysql_fetch_row(result.get());
                if (row == NULL || row[0] == NULL || row[1] == NULL)
                {
                    return -1;
                }
                char *end;
                errno = 0;
                long localId = strtol(row[0], &end, 10);
                if (errno != 0 || end == row[0] || *end != '\0' || localId <= 0 || localId > INT_MAX)
                {
                    return -1;
                }
                errno = 0;
                long links = strtol(row[1], &end, 10);
                if (errno != 0 || end == row[1] || *end != '\0' || links <= 0 || links > INT_MAX)
                {
                    return -1;
                }
                result.reset();
                ret = snprintf(query, sizeof(query),
                               "update local_files set link_num = link_num - 1 where id = %ld and link_num > 0", localId);
                if (ret < 0 || ret >= (int)sizeof(query))
                {
                    return -1;
                }
                if (mysql_query(conn, query) != 0)
                {
                    return -1;
                }
                if (mysql_affected_rows(conn) != 1)
                {
                    return -1;
                }
            }
        }
        ret = snprintf(query, sizeof(query),
                       "delete from virtual_file where id = %d and owner_id = %d", node.id, user_id);
        if (ret < 0 || ret >= (int)sizeof(query))
        {
            return -1;
        }
        if (mysql_query(conn, query) != 0)
        {
            return -1;
        }
        if (mysql_affected_rows(conn) != 1)
        {
            return -1;
        }
    }
    return transaction.commit();
}

// 获得父结点的id
int get_parent_id(int id, int *pparent_code)
{
    MysqlConnPtr connection = open_connection();
    MYSQL *conn = connection.get();
    if (!conn)
    {
        return -1;
    }
    int ret = mysql_query(conn, "set names 'utf8'");
    if (ret != 0)
    {
        fprintf(stderr, "Database query failed.\n");
        return -1;
    }
    char query[1024] = {0};
    ret = snprintf(query, sizeof(query), "select parent_id from virtual_file where id = %d", id);
    if (ret < 0 || ret >= (int)sizeof(query))
    {
        return -1;
    }

    ret = do_query(query, conn);
    if (ret != 0)
    {
        return -1;
    }
    MysqlResPtr result(mysql_store_result(conn));
    if (result == NULL)
    {
        fprintf(stderr, "Database query failed.\n");
        return -1;
    }
    int rows = mysql_num_rows(result.get());

    if (rows == 0)// 文件不存在返回1
    {
        return 1;
    }
    MYSQL_ROW row;
    if ((row = mysql_fetch_row(result.get())) == NULL)
    {
        if (mysql_errno(conn) != 0)
        {
            return -1;
        }
        return 1;
    }
    if (row[0] == NULL)
    {
        return -1;
    }
    char *end;
    errno = 0;
    long value = strtol(row[0], &end, 10);
    if (errno != 0 || end == row[0] || *end != '\0' || value < 0 || value > INT_MAX)
    {
        return -1;
    }
    *pparent_code = value;
    return 0;
}
