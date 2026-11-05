#include "threadPool.h"
#include "worker.h"

//连接数据库
int mysqlconnect(MYSQL **pconn)
{
    *pconn = NULL;
    const char *host = getenv("NET_DISK_DB_HOST");
    const char *user = getenv("NET_DISK_DB_USER");
    const char *passwd = getenv("NET_DISK_DB_PASSWORD");
    const char *db = getenv("NET_DISK_DB_NAME");

    if(host == NULL || host[0] == '\0')
    {
        host = "localhost";
    }
    if(user == NULL || user[0] == '\0')
    {
        user = "net_disk_app";
    }
    if(passwd == NULL || passwd[0] == '\0')
    {
        fprintf(stderr, "NET_DISK_DB_PASSWORD is not set\n");
        return EXIT_FAILURE;
    }
    if(db == NULL || db[0] == '\0')
    {
        db = "net_disk";
    }

    *pconn = mysql_init(NULL);
    if(*pconn == NULL)
    {
        fprintf(stderr, "mysql_init failed\n");
        return EXIT_FAILURE;
    }
    if(mysql_real_connect(*pconn, host, user, passwd, db, 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "Database connection failed.\n");
        mysql_close(*pconn);
        *pconn = NULL;
        return EXIT_FAILURE;
    }
    return 0;
}

//执行sql语句
int do_query(char *query, MYSQL **pconn)
{
    if(*pconn == NULL)
    {
        return EXIT_FAILURE;
    }
    int ret = mysql_query(*pconn, query);
    if(ret != 0)
    {
        fprintf(stderr, "Database query failed.\n");
        return EXIT_FAILURE;
    }
    return 0;
}

// 外部字符串通过参数绑定进入SQL，结果截断或NULL均按失败处理
static int execute_sql(const char *query, const char **values, int count,
                       char **columns, const size_t *capacities, int columnCount)
{
    MYSQL *conn = NULL;
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[5];
    MYSQL_BIND outputs[3];
    unsigned long lengths[3] = {0};
    my_bool nulls[3] = {0};
    my_bool errors[3] = {0};
    int status = -1;
    if(count < 0 || count > 5 || columnCount < 0 || columnCount > 3 ||
       mysqlconnect(&conn) != 0)
    {
        goto done;
    }
    if(mysql_set_character_set(conn, "utf8") != 0)
    {
        goto done;
    }
    stmt = mysql_stmt_init(conn);
    if(stmt == NULL || mysql_stmt_prepare(stmt, query, strlen(query)) != 0 ||
       mysql_stmt_param_count(stmt) != (unsigned long)count)
    {
        goto done;
    }
    bzero(params, sizeof(params));
    for(int i = 0; i < count; ++i)
    {
        if(values[i] == NULL)
        {
            goto done;
        }
        params[i].buffer_type = MYSQL_TYPE_STRING;
        params[i].buffer = (void*)values[i];
        params[i].buffer_length = strlen(values[i]);
    }
    if((count > 0 && mysql_stmt_bind_param(stmt, params) != 0) ||
       mysql_stmt_execute(stmt) != 0)
    {
        goto done;
    }
    if(columnCount == 0)
    {
        status = 0;
        goto done;
    }
    if(mysql_stmt_field_count(stmt) != (unsigned int)columnCount)
    {
        goto done;
    }
    bzero(outputs, sizeof(outputs));
    for(int i = 0; i < columnCount; ++i)
    {
        if(columns[i] == NULL || capacities[i] == 0)
        {
            goto done;
        }
        outputs[i].buffer_type = MYSQL_TYPE_STRING;
        outputs[i].buffer = columns[i];
        outputs[i].buffer_length = capacities[i] - 1;
        outputs[i].length = &lengths[i];
        outputs[i].is_null = &nulls[i];
        outputs[i].error = &errors[i];
    }
    if(mysql_stmt_bind_result(stmt, outputs) != 0 || mysql_stmt_store_result(stmt) != 0)
    {
        goto done;
    }
    int ret = mysql_stmt_fetch(stmt);
    if(ret == MYSQL_NO_DATA)
    {
        status = 1;
        goto done;
    }
    if(ret != 0)
    {
        goto done;
    }
    for(int i = 0; i < columnCount; ++i)
    {
        if(nulls[i] || errors[i] || lengths[i] >= capacities[i])
        {
            goto done;
        }
        columns[i][lengths[i]] = '\0';
    }
    status = 0;
done:
    if(stmt != NULL)
    {
        mysql_stmt_close(stmt);
    }
    if(conn != NULL)
    {
        mysql_close(conn);
    }
    if(status < 0)
    {
        fprintf(stderr, "Database statement failed.\n");
    }
    return status;
}

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
    MYSQL *conn = NULL;
    MYSQL_RES *result = NULL;
    int ret = mysqlconnect(&conn);
    if(ret != 0)
    {
        goto error;
    }
    ret = mysql_query(conn, "set names 'utf8'");
    if(ret != 0)
    {
        fprintf(stderr, "Database query failed.\n");
        goto error;
    }
    ret = do_query("select username, salt, cryptpasswd from client_info", &conn);
    if(ret != 0)
    {
        goto error;
    }
    result = mysql_use_result(conn);
    if(result == NULL)
    {
        fprintf(stderr, "Database query failed.\n");
        goto error;
    }
    MYSQL_ROW row;
    // printf("user_name = %s\n", user_name);
    while((row = mysql_fetch_row(result)) != NULL)
    {
        if(row[0] == NULL || row[1] == NULL || row[2] == NULL)
        {
            goto error;
        }
        // printf("row[0] = %s\n", row[0]);
        if(strcmp(row[0], user_name) == 0)
        {
            if(strlen(row[1]) >= SATL_LEN + 1)
            {
                goto error;
            }
            strcpy(salt, row[1]);
            if(strlen(row[2]) >= 50)
            {
                goto error;
            }
            strcpy(crptpasswd, row[2]);
            mysql_free_result(result);
            result = NULL;
            mysql_close(conn);
            return 0;
        }
    }
    if(mysql_errno(conn) != 0)
    {
        goto error;
    }
    mysql_free_result(result);
    result = NULL;
    mysql_close(conn);
    return 1;

error:
    if(result != NULL)
    {
        mysql_free_result(result);
    }
    if(conn != NULL)
    {
        mysql_close(conn);
    }
    return -1;
}

// 根据用户名获取用户id
int find_user_id(char *user_name, int *puser_id)
{
    MYSQL *conn = NULL;
    MYSQL_RES *result = NULL;
    int ret = mysqlconnect(&conn);
    if(ret != 0)
    {
        goto error;
    }
    ret = mysql_query(conn, "set names 'utf8'");
    if(ret != 0)
    {
        fprintf(stderr, "Database query failed.\n");
        goto error;
    }
    ret = do_query("select username, id from client_info", &conn);
    if(ret != 0)
    {
        goto error;
    }
    result = mysql_use_result(conn);
    if(result == NULL)
    {
        fprintf(stderr, "Database query failed.\n");
        goto error;
    }
    MYSQL_ROW row;
    while((row = mysql_fetch_row(result)) != NULL)
    {
        if(row[0] == NULL || row[1] == NULL)
        {
            goto error;
        }
        if(strcmp(row[0], user_name) == 0)
        {
            char *end;
            errno = 0;
            long value = strtol(row[1], &end, 10);
            if(errno != 0 || end == row[1] || *end != '\0' || value < 0 || value > INT_MAX)
            {
                goto error;
            }
            *puser_id = value;

            mysql_free_result(result);
            result = NULL;
            mysql_close(conn);
            return 0;
        }
    }
    if(mysql_errno(conn) != 0)
    {
        goto error;
    }
    mysql_free_result(result);
    result = NULL;
    mysql_close(conn);
    return 1;

error:
    if(result != NULL)
    {
        mysql_free_result(result);
    }
    if(conn != NULL)
    {
        mysql_close(conn);
    }
    return -1;
}

// 根据用户id获取用户名
int get_user_name(char *user_name, const int user_id)
{
    MYSQL *conn = NULL;
    MYSQL_RES *result = NULL;
    int ret = mysqlconnect(&conn);
    if(ret != 0)
    {
        goto error;
    }
    ret = mysql_query(conn, "set names 'utf8'");
    if(ret != 0)
    {
        fprintf(stderr, "Database query failed.\n");
        goto error;
    }
    ret = do_query("select username, id from client_info", &conn);
    if(ret != 0)
    {
        goto error;
    }
    result = mysql_use_result(conn);
    if(result == NULL)
    {
        fprintf(stderr, "Database query failed.\n");
        goto error;
    }
    MYSQL_ROW row;
    while((row = mysql_fetch_row(result)) != NULL)
    {
        if(row[1] == NULL || row[0] == NULL)
        {
            goto error;
        }
        if(user_id == atoi(row[1]))     //如果userid符合，直接拷贝并返回
        {
            if(strlen(row[0]) >= PROTOCOL_USER_NAME_CAPACITY)
            {
                goto error;
            }
            strcpy(user_name, row[0]);

            mysql_free_result(result);
            result = NULL;
            mysql_close(conn);
            return 0;
        }
    }
    if(mysql_errno(conn) != 0)
    {
        goto error;
    }
    mysql_free_result(result);
    result = NULL;
    mysql_close(conn);
    return 1;

error:
    if(result != NULL)
    {
        mysql_free_result(result);
    }
    if(conn != NULL)
    {
        mysql_close(conn);
    }
    return -1;
}

// 通过user_id获取当前目录的文件夹编号
int get_user_code(int user_id, int *pcode)
{
    MYSQL *conn = NULL;
    MYSQL_RES *result = NULL;
    int ret = mysqlconnect(&conn);
    if(ret != 0)
    {
        goto error;
    }
    ret = do_query("select code, id from client_info", &conn);
    if(ret != 0)
    {
        goto error;
    }
    result = mysql_use_result(conn);
    if(result == NULL)
    {
        fprintf(stderr, "Database query failed.\n");
        goto error;
    }
    MYSQL_ROW row;
    while((row = mysql_fetch_row(result)) != NULL)
    {
        if(row[1] == NULL || row[0] == NULL)
        {
            goto error;
        }
        if(user_id == atoi(row[1]))
        {
            char *end;
            errno = 0;
            long value = strtol(row[0], &end, 10);
            if(errno != 0 || end == row[0] || *end != '\0' || value < 0 || value > INT_MAX)
            {
                goto error;
            }
            *pcode = value;

            mysql_free_result(result);
            result = NULL;
            mysql_close(conn);
            return 0;
        }
    }
    if(mysql_errno(conn) != 0)
    {
        goto error;
    }
    mysql_free_result(result);
    result = NULL;
    mysql_close(conn);
    return 1;

error:
    if(result != NULL)
    {
        mysql_free_result(result);
    }
    if(conn != NULL)
    {
        mysql_close(conn);
    }
    return -1;
}

// 通过user_id获取当前目录的字符串
int get_user_pwd(int user_id, char *pwd)
{
    MYSQL *conn = NULL;
    MYSQL_RES *result = NULL;
    int ret = mysqlconnect(&conn);
    if(ret != 0)
    {
        goto error;
    }
    ret = mysql_query(conn, "set names 'utf8'");
    if(ret != 0)
    {
        fprintf(stderr, "Database query failed.\n");
        goto error;
    }
    ret = do_query("select pwd, id from client_info", &conn);
    if(ret != 0)
    {
        goto error;
    }
    result = mysql_use_result(conn);
    if(result == NULL)
    {
        fprintf(stderr, "Database query failed.\n");
        goto error;
    }
    MYSQL_ROW row;
    while((row = mysql_fetch_row(result)) != NULL)
    {
        if(row[1] == NULL || row[0] == NULL)
        {
            goto error;
        }
        if(user_id == atoi(row[1]))
        {
            if(strlen(row[0]) >= 1000)
            {
                goto error;
            }
            strcpy(pwd, row[0]);

            mysql_free_result(result);
            result = NULL;
            mysql_close(conn);
            return 0;
        }
    }
    if(mysql_errno(conn) != 0)
    {
        goto error;
    }
    mysql_free_result(result);
    result = NULL;
    mysql_close(conn);
    return 1;

error:
    if(result != NULL)
    {
        mysql_free_result(result);
    }
    if(conn != NULL)
    {
        mysql_close(conn);
    }
    return -1;
}

// 通过user_id更新用户当前目录
int update_user_pwd(int user_id, char *pwd, int code)
{
    char id[32], codeText[32];
    snprintf(id, sizeof(id), "%d", user_id);
    snprintf(codeText, sizeof(codeText), "%d", code);
    const char *values[] = {pwd, codeText, id};
    return execute_sql("update client_info set pwd = ?, code = ? where id = ?",
                       values, 3, NULL, NULL, 0);
}

// 初始化本地文件表
int init_local_file()
{
    DIR *dirp = opendir("NetDisk");
    if(dirp == NULL)
    {
        return -1;
    }
    int status = -1;
    struct dirent *pdirent;
    while(1)
    {
        errno = 0;
        pdirent = readdir(dirp);
        if(pdirent == NULL)
        {
            status = errno == 0 ? 0 : -1;
            break;
        }
        if(pdirent->d_name[0] == '.')
        {
            continue;
        }
        const char *values[] = {pdirent->d_name};
        char md5_str[MD5_STR_LEN + 1] = {0};
        char *columns[] = {md5_str};
        size_t capacities[] = {sizeof(md5_str)};
        int ret = execute_sql("select md5 from local_files where real_file_name = ?",
                              values, 1, columns, capacities, 1);
        if(ret < 0)
        {
            break;
        }
        if(ret == 0)
        {
            continue;
        }
        if(Compute_file_md5(pdirent->d_name, md5_str) != 0)
        {
            break;
        }
        // 已有相同内容时保留未登记的物理文件，不插入重复 MD5
        ret = if_local_exist(md5_str);
        if(ret < 0)
        {
            break;
        }
        if(ret == 0)
        {
            continue;
        }
        const char *insert[] = {md5_str, pdirent->d_name, md5_str};
        if(execute_sql("insert into local_files(md5, link_num, real_file_name) "
                       "select ?, count(*), ? from virtual_file where md5 = ? and type = 'f'",
                       insert, 3, NULL, NULL, 0) != 0)
        {
            break;
        }
    }
    closedir(dirp);
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
    if(ret != 0)
    {
        return ret;
    }
    if(strcmp(status, "0") == 0)
    {
        *complete = 0;
    }
    else if(strcmp(status, "1") == 0)
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
    if(complete != 0 && complete != 1)
    {
        return -1;
    }
    const char *values[] = {complete == 0 ? "0" : "1", md5_str, real_name};
    if(execute_sql("update local_files set upload_complete = ? where md5 = ? and real_file_name = ?",
                   values, 3, NULL, NULL, 0) != 0)
    {
        return -1;
    }
    int actual;
    if(get_local_file_status(md5_str, real_name, &actual) != 0 || actual != complete)
    {
        return -1;
    }
    return 0;
}

// 增加本地文件的链接数
int add_link(char *md5_str)
{
    MYSQL *conn = NULL;
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[2];
    MYSQL_BIND output;
    my_bool isNull = 0;
    int localId = 0;
    int maxLink = INT_MAX;
    int inTransaction = 0;
    const char *query = "select id from local_files where md5 = ? for update";
    if(md5_str == NULL || mysqlconnect(&conn) != 0 ||
       mysql_set_character_set(conn, "utf8") != 0 || mysql_autocommit(conn, 0) != 0)
    {
        goto error;
    }
    inTransaction = 1;
    stmt = mysql_stmt_init(conn);
    if(stmt == NULL || mysql_stmt_prepare(stmt, query, strlen(query)) != 0)
    {
        goto error;
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
    if(mysql_stmt_bind_param(stmt, params) != 0 || mysql_stmt_execute(stmt) != 0 ||
       mysql_stmt_bind_result(stmt, &output) != 0 || mysql_stmt_store_result(stmt) != 0 ||
       mysql_stmt_num_rows(stmt) != 1 || mysql_stmt_fetch(stmt) != 0 ||
       isNull || localId <= 0)
    {
        goto error;
    }
    int ret = mysql_stmt_close(stmt);
    stmt = NULL;
    if(ret != 0)
    {
        goto error;
    }

    // 在数据库内直接加一，避免使用并发操作前读取的旧值
    query = "update local_files set link_num = link_num + 1 "
            "where id = ? and link_num >= 0 and link_num < ?";
    stmt = mysql_stmt_init(conn);
    if(stmt == NULL || mysql_stmt_prepare(stmt, query, strlen(query)) != 0)
    {
        goto error;
    }
    bzero(params, sizeof(params));
    params[0].buffer_type = MYSQL_TYPE_LONG;
    params[0].buffer = &localId;
    params[0].buffer_length = sizeof(localId);
    params[1].buffer_type = MYSQL_TYPE_LONG;
    params[1].buffer = &maxLink;
    params[1].buffer_length = sizeof(maxLink);
    if(mysql_stmt_bind_param(stmt, params) != 0 || mysql_stmt_execute(stmt) != 0 ||
       mysql_stmt_affected_rows(stmt) != 1)
    {
        goto error;
    }
    ret = mysql_stmt_close(stmt);
    stmt = NULL;
    if(ret != 0 || mysql_commit(conn) != 0)
    {
        goto error;
    }
    mysql_close(conn);
    return 0;

error:
    if(stmt != NULL)
    {
        mysql_stmt_close(stmt);
    }
    if(conn != NULL)
    {
        if(inTransaction)
        {
            mysql_rollback(conn);
        }
        mysql_close(conn);
    }
    fprintf(stderr, "Database statement failed.\n");
    return -1;
}

// 减少本地文件的链接数
int minus_link(char *md5_str)
{
    const char *values[] = {md5_str};
    char number[32];
    char *columns[] = {number};
    size_t capacities[] = {sizeof(number)};
    if(execute_sql("select link_num from local_files where md5 = ?",
                   values, 1, columns, capacities, 1) != 0)
    {
        return -1;
    }
    char *end;
    errno = 0;
    long link_num = strtol(number, &end, 10);
    if(errno != 0 || end == number || *end != '\0' || link_num < 0 ||
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
static int upload_statement(MYSQL *conn, const char *query, const char **values, int count,
                            char **columns, const size_t *capacities, int columnCount,
                            my_ulonglong *rows)
{
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[5], outputs[3];
    unsigned long lengths[3] = {0};
    my_bool nulls[3] = {0}, errors[3] = {0};
    int ignoredId = 0;
    int status = -1;
    if(count < 0 || count > 5 || columnCount < 0 || columnCount > 3)
    {
        return -1;
    }
    stmt = mysql_stmt_init(conn);
    if(stmt == NULL || mysql_stmt_prepare(stmt, query, strlen(query)) != 0 ||
       mysql_stmt_param_count(stmt) != (unsigned long)count)
    {
        goto done;
    }
    bzero(params, sizeof(params));
    for(int i = 0; i < count; ++i)
    {
        if(values[i] == NULL)
        {
            goto done;
        }
        params[i].buffer_type = MYSQL_TYPE_STRING;
        params[i].buffer = (void *)values[i];
        params[i].buffer_length = strlen(values[i]);
    }
    if((count > 0 && mysql_stmt_bind_param(stmt, params) != 0) ||
       mysql_stmt_execute(stmt) != 0)
    {
        goto done;
    }
    if(mysql_stmt_field_count(stmt) == 0)
    {
        *rows = mysql_stmt_affected_rows(stmt);
        status = 0;
        goto done;
    }
    if((columnCount == 0 && mysql_stmt_field_count(stmt) != 1) ||
       (columnCount > 0 && mysql_stmt_field_count(stmt) != (unsigned int)columnCount))
    {
        goto done;
    }
    bzero(outputs, sizeof(outputs));
    if(columnCount == 0)
    {
        // 仅统计记录数的查询都返回 id，也先绑定结果以兼容旧客户端库
        outputs[0].buffer_type = MYSQL_TYPE_LONG;
        outputs[0].buffer = &ignoredId;
        outputs[0].buffer_length = sizeof(ignoredId);
    }
    for(int i = 0; i < columnCount; ++i)
    {
        if(columns[i] == NULL || capacities[i] == 0)
        {
            goto done;
        }
        outputs[i].buffer_type = MYSQL_TYPE_STRING;
        outputs[i].buffer = columns[i];
        outputs[i].buffer_length = capacities[i] - 1;
        outputs[i].length = &lengths[i];
        outputs[i].is_null = &nulls[i];
        outputs[i].error = &errors[i];
    }
    if(mysql_stmt_bind_result(stmt, outputs) != 0 ||
       mysql_stmt_store_result(stmt) != 0)
    {
        goto done;
    }
    *rows = mysql_stmt_num_rows(stmt);
    if(*rows == 1 && columnCount > 0)
    {
        if(mysql_stmt_fetch(stmt) != 0)
        {
            goto done;
        }
        for(int i = 0; i < columnCount; ++i)
        {
            if(nulls[i] || errors[i] || lengths[i] >= capacities[i])
            {
                goto done;
            }
            columns[i][lengths[i]] = '\0';
        }
    }
    status = 0;
done:
    if(stmt != NULL && mysql_stmt_close(stmt) != 0)
    {
        status = -1;
    }
    return status;
}

// 准备上传：文件锁和数据库记录一并确定，网络传输在提交事务后进行
int prepare_upload(int parent_id, char *name, int owner_id, char *md5_str,
                   int64_t fileSize, char *real_name, int *pfd, int *complete)
{
    MYSQL *conn = NULL;
    int fd = -1, transaction = 0, status = -1;
    int oldFile = 0, localExists = 0, localComplete = 0;
    my_ulonglong rows;
    char owner[32], parent[32], size[32], oldMd5[MD5_STR_LEN + 1];
    char oldSize[32], oldType[2], localStatus[16], localLinks[32];
    char *virtualColumns[] = {oldMd5, oldSize, oldType};
    size_t virtualCapacities[] = {sizeof(oldMd5), sizeof(oldSize), sizeof(oldType)};
    char *localColumns[] = {real_name, localStatus};
    size_t localCapacities[] = {1024, sizeof(localStatus)};
    *pfd = -1;
    *complete = 0;
    if(parent_id < 0 || owner_id < 0 || fileSize < 0 ||
       !is_valid_file_name(name, strlen(name)) || strlen(name) > 255 ||
       strlen(md5_str) != MD5_STR_LEN ||
       strspn(md5_str, "0123456789abcdefABCDEF") != MD5_STR_LEN)
    {
        return -1;
    }
    snprintf(owner, sizeof(owner), "%d", owner_id);
    snprintf(parent, sizeof(parent), "%d", parent_id);
    snprintf(size, sizeof(size), "%lld", (long long)fileSize);
    if(mysqlconnect(&conn) != 0 || mysql_set_character_set(conn, "utf8") != 0 ||
       mysql_autocommit(conn, 0) != 0)
    {
        goto done;
    }
    transaction = 1;
    const char *userValues[] = {owner};
    if(upload_statement(conn, "select id from client_info where id = ? for update",
                        userValues, 1, NULL, NULL, 0, &rows) != 0 || rows != 1)
    {
        goto done;
    }
    if(parent_id != 0)
    {
        const char *parentValues[] = {parent, owner};
        if(upload_statement(conn, "select id from virtual_file where id = ? and owner_id = ? and type = 'd' for update",
                            parentValues, 2, NULL, NULL, 0, &rows) != 0 || rows != 1)
        {
            goto done;
        }
    }
    const char *virtualValues[] = {owner, parent, name};
    if(upload_statement(conn, "select md5, filesize, type from virtual_file where owner_id = ? and parent_id = ? and filename = ? for update",
                        virtualValues, 3, virtualColumns, virtualCapacities, 3, &rows) != 0 || rows > 1)
    {
        goto done;
    }
    if(rows == 1)
    {
        char *end;
        errno = 0;
        long long oldValue = strtoll(oldSize, &end, 10);
        if(errno != 0 || end == oldSize || *end != '\0' || oldValue < 0 ||
           (int64_t)oldValue != oldValue)
        {
            goto done;
        }
        if(strcmp(oldType, "f") != 0 || strcmp(oldMd5, md5_str) != 0 || oldValue != fileSize)
        {
            status = 1;
            goto done;
        }
        oldFile = 1;
    }
    const char *md5Values[] = {md5_str};
    const char *sizeValues[] = {md5_str, size};
    // 同一物理内容的已登记大小不同，不能用本次大小改写共享文件状态
    if(upload_statement(conn, "select id from virtual_file where md5 = ? and type = 'f' and (filesize is null or filesize <> CAST(? AS SIGNED)) for update",
                        sizeValues, 2, NULL, NULL, 0, &rows) != 0)
    {
        goto done;
    }
    if(rows != 0)
    {
        status = 1;
        goto done;
    }
    if(upload_statement(conn, "select real_file_name, upload_complete from local_files where md5 = ? for update",
                        md5Values, 1, localColumns, localCapacities, 2, &rows) != 0 || rows > 1)
    {
        goto done;
    }
    localExists = rows == 1;
    if(localExists)
    {
        if(strcmp(localStatus, "0") == 0)
        {
            localComplete = 0;
        }
        else if(strcmp(localStatus, "1") == 0)
        {
            localComplete = 1;
        }
        else
        {
            goto done;
        }
        fd = open_in_root("NetDisk", real_name, O_RDWR, 0);
        if(fd == -1 && errno == ENOENT)
        {
            fd = open_in_root("NetDisk", real_name, O_RDWR | O_CREAT | O_EXCL, 0666);
            localComplete = 0;
        }
        if(fd == -1 || flock(fd, LOCK_EX | LOCK_NB) == -1)
        {
            goto done;
        }
        struct stat statbuf;
        if(fstat(fd, &statbuf) == -1)
        {
            goto done;
        }
        if(localComplete == 1 && statbuf.st_size != fileSize)
        {
            localComplete = 0;
        }
        if(localComplete == 0)
        {
            const char *stateValues[] = {md5_str, real_name};
            if(upload_statement(conn, "update local_files set upload_complete = 0 where md5 = ? and real_file_name = ?",
                                stateValues, 2, NULL, NULL, 0, &rows) != 0)
            {
                goto done;
            }
        }
    }
    else
    {
        // 历史记录可能已有虚拟引用但缺少本地记录，先数出已有引用
        if(upload_statement(conn, "select id from virtual_file where md5 = ? and type = 'f' for update",
                            md5Values, 1, NULL, NULL, 0, &rows) != 0 ||
           rows > (my_ulonglong)INT_MAX - (oldFile ? 0 : 1))
        {
            goto done;
        }
        snprintf(localLinks, sizeof(localLinks), "%llu", (unsigned long long)rows + (oldFile ? 0 : 1));
        int length = snprintf(real_name, 1024, "%s%d%d", name, owner_id, parent_id);
        if(length < 0 || length > 255 || !is_valid_file_name(real_name, length))
        {
            goto done;
        }
        const char *localValues[] = {md5_str, localLinks, real_name};
        if(upload_statement(conn, "insert into local_files(md5, link_num, real_file_name, upload_complete) values(?, ?, ?, 0)",
                            localValues, 3, NULL, NULL, 0, &rows) != 0 || rows != 1)
        {
            goto done;
        }
        fd = open_in_root("NetDisk", real_name, O_RDWR | O_CREAT | O_EXCL, 0666);
        if(fd == -1 || flock(fd, LOCK_EX | LOCK_NB) == -1)
        {
            goto done;
        }
    }
    if(!oldFile)
    {
        const char *newValues[] = {parent, name, owner, md5_str, size};
        if(upload_statement(conn, "insert into virtual_file(parent_id, filename, owner_id, md5, filesize, type) values(?, ?, ?, ?, ?, 'f')",
                            newValues, 5, NULL, NULL, 0, &rows) != 0 || rows != 1)
        {
            goto done;
        }
        if(localExists && (upload_statement(conn, "update local_files set link_num = link_num + 1 where md5 = ? and link_num >= 0 and link_num < 2147483647",
                                            md5Values, 1, NULL, NULL, 0, &rows) != 0 || rows != 1))
        {
            goto done;
        }
    }
    if(mysql_commit(conn) != 0)
    {
        goto done;
    }
    transaction = 0;
    *pfd = fd;
    *complete = localComplete;
    fd = -1;
    status = 0;
done:
    if(conn != NULL)
    {
        if(transaction)
        {
            mysql_rollback(conn);
        }
        mysql_close(conn);
    }
    if(fd != -1)
    {
        close(fd);
    }
    return status;
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
int get_file_code(int user_id, char *filename, int *pcode, int parent_id, char *type)
{
    char owner[32], parent[32], id[32];
    snprintf(owner, sizeof(owner), "%d", user_id);
    snprintf(parent, sizeof(parent), "%d", parent_id);
    const char *values[] = {owner, filename, parent};
    char *columns[] = {id, type};
    size_t capacities[] = {sizeof(id), 2};
    int ret = execute_sql("select id, type from virtual_file where owner_id = ? and filename = ? and parent_id = ?",
                          values, 3, columns, capacities, 2);
    if(ret != 0)
    {
        return ret;
    }
    char *end;
    errno = 0;
    long code = strtol(id, &end, 10);
    if(errno != 0 || end == id || *end != '\0' || code < 0 || code > INT_MAX)
    {
        return -1;
    }
    *pcode = code;
    return 0;
}

// 增加虚拟文件夹
int add_virtual_dir(int parent_id, char *filename, int owner_id)
{
    char parent[32], owner[32];
    snprintf(parent, sizeof(parent), "%d", parent_id);
    snprintf(owner, sizeof(owner), "%d", owner_id);
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
int find_code_files(connection_t *pConnection, int code, int user_id)
{
    MYSQL *conn = NULL;
    MYSQL_RES *result = NULL;
    int ret = mysqlconnect(&conn);
    if(ret != 0)
    {
        goto error;
    }
    ret = mysql_query(conn, "set names 'utf8'");
    if(ret != 0)
    {
        fprintf(stderr, "Database query failed.\n");
        goto error;
    }
    char query[1024] = {0};
    //可以通过parent_id找到目录下的所有文件
    ret = snprintf(query, sizeof(query),
                   "select v.filename from virtual_file v where v.parent_id = %d and v.owner_id = %d "
                   "and (v.type = 'd' or (v.type = 'f' and exists "
                   "(select 1 from local_files l where l.md5 = v.md5 and l.upload_complete = 1)))",
                   code, user_id);
    if(ret < 0 || ret >= (int)sizeof(query))
    {
        goto error;
    }
    ret = do_query(query, &conn);
    if(ret != 0)
    {
        goto error;
    }
    result = mysql_use_result(conn);
    if(result == NULL)
    {
        fprintf(stderr, "Database query failed.\n");
        goto error;
    }
    MYSQL_ROW row;
    train_t train;
    while((row = mysql_fetch_row(result)) != NULL)
    {
        if(row[0] == NULL)
        {
            goto error;
        }
        train.length = strlen(row[0]);
        bzero(train.buf, sizeof(train.buf));
        if(strlen(row[0]) >= sizeof(train.buf))
        {
            goto error;
        }
        strcpy(train.buf, row[0]);
        //每找到一个文件，就发给客户端
        if(wire_queue_train(pConnection, &train) != (int)sizeof(train.length) + train.length)
        {
            goto error;
        }
    }
    if(mysql_errno(conn) != 0)
    {
        goto error;
    }
    // 发送长度0表示结束
    train.length = 0;
    if(wire_queue_train(pConnection, &train) != (int)sizeof(train.length) + train.length)
    {
        goto error;
    }
    mysql_free_result(result);
    result = NULL;
    mysql_close(conn);
    return 0;

error:
    if(result != NULL)
    {
        mysql_free_result(result);
    }
    if(conn != NULL)
    {
        mysql_close(conn);
    }
    return -1;
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
    if(ret != 0)
    {
        return ret;
    }
    char *end;
    errno = 0;
    long long value = strtoll(number, &end, 10);
    if(errno != 0 || end == number || *end != '\0' || value < 0 ||
       (int64_t)value != value)
    {
        return -1;
    }
    *size = (int64_t)value;
    return 0;
}

typedef struct delete_node_s {
    int id;
    char type;
    char md5[MD5_STR_LEN + 1];
} delete_node_t;

// 保存查询到的子项，避免目录层级过深时递归耗尽栈空间
static int collect_delete_nodes(MYSQL *conn, MYSQL_RES *result,
                                delete_node_t **nodes, size_t *count, size_t *capacity)
{
    MYSQL_ROW row;
    while((row = mysql_fetch_row(result)) != NULL)
    {
        if(row[0] == NULL || row[1] == NULL ||
           (strcmp(row[1], "f") != 0 && strcmp(row[1], "d") != 0))
        {
            return -1;
        }
        char *end;
        errno = 0;
        long id = strtol(row[0], &end, 10);
        if(errno != 0 || end == row[0] || *end != '\0' || id <= 0 || id > INT_MAX)
        {
            return -1;
        }
        if(row[1][0] == 'f' && (row[2] == NULL || strlen(row[2]) != MD5_STR_LEN ||
           strspn(row[2], "0123456789abcdefABCDEF") != MD5_STR_LEN))
        {
            return -1;
        }
        // parent_id 异常成环时停止删除，避免重复处理同一记录
        for(size_t i = 0; i < *count; ++i)
        {
            if((*nodes)[i].id == id)
            {
                return -1;
            }
        }
        if(*count == *capacity)
        {
            size_t newCapacity = *capacity == 0 ? 16 : *capacity * 2;
            if(newCapacity < *capacity || newCapacity > SIZE_MAX / sizeof(**nodes))
            {
                return -1;
            }
            delete_node_t *newNodes = realloc(*nodes, newCapacity * sizeof(**nodes));
            if(newNodes == NULL)
            {
                return -1;
            }
            *nodes = newNodes;
            *capacity = newCapacity;
        }
        (*nodes)[*count].id = id;
        (*nodes)[*count].type = row[1][0];
        (*nodes)[*count].md5[0] = '\0';
        if(row[1][0] == 'f')
        {
            strcpy((*nodes)[*count].md5, row[2]);
        }
        ++*count;
    }
    return mysql_errno(conn) == 0 ? 0 : -1;
}

// 删除目标及其后代，在同一事务中更新文件引用数
int del_virtual_file(int code, int user_id)
{
    MYSQL *conn = NULL;
    MYSQL_RES *result = NULL;
    delete_node_t *nodes = NULL;
    size_t count = 0, capacity = 0;
    int inTransaction = 0;
    char query[1024];
    if(code <= 0 || user_id < 0 || mysqlconnect(&conn) != 0 ||
       mysql_query(conn, "set names 'utf8'") != 0 || mysql_autocommit(conn, 0) != 0)
    {
        goto error;
    }
    inTransaction = 1;

    int ret = snprintf(query, sizeof(query),
                       "select id, type, md5 from virtual_file where id = %d and owner_id = %d for update",
                       code, user_id);
    if(ret < 0 || ret >= (int)sizeof(query) || mysql_query(conn, query) != 0 ||
       (result = mysql_store_result(conn)) == NULL || mysql_num_rows(result) != 1 ||
       collect_delete_nodes(conn, result, &nodes, &count, &capacity) != 0)
    {
        goto error;
    }
    mysql_free_result(result);
    result = NULL;

    // 逐层收集，子项总在父项之后，反向遍历即可从内向外删除
    for(size_t i = 0; i < count; ++i)
    {
        ret = snprintf(query, sizeof(query),
                       "select id, type, md5 from virtual_file where parent_id = %d and owner_id = %d for update",
                       nodes[i].id, user_id);
        if(ret < 0 || ret >= (int)sizeof(query) || mysql_query(conn, query) != 0 ||
           (result = mysql_store_result(conn)) == NULL ||
           collect_delete_nodes(conn, result, &nodes, &count, &capacity) != 0)
        {
            goto error;
        }
        mysql_free_result(result);
        result = NULL;
    }
    for(size_t i = count; i > 0; --i)
    {
        delete_node_t *node = &nodes[i - 1];
        if(node->type == 'f')
        {
            // MD5 已校验为固定长度的十六进制字符，不含 SQL 特殊字符
            ret = snprintf(query, sizeof(query),
                           "select id, link_num from local_files where md5 = '%s' for update", node->md5);
            if(ret < 0 || ret >= (int)sizeof(query) || mysql_query(conn, query) != 0 ||
               (result = mysql_store_result(conn)) == NULL || mysql_num_rows(result) > 1)
            {
                goto error;
            }
            if(mysql_num_rows(result) == 1)
            {
                MYSQL_ROW row = mysql_fetch_row(result);
                if(row == NULL || row[0] == NULL || row[1] == NULL)
                {
                    goto error;
                }
                char *end;
                errno = 0;
                long localId = strtol(row[0], &end, 10);
                if(errno != 0 || end == row[0] || *end != '\0' || localId <= 0 || localId > INT_MAX)
                {
                    goto error;
                }
                errno = 0;
                long links = strtol(row[1], &end, 10);
                if(errno != 0 || end == row[1] || *end != '\0' || links <= 0 || links > INT_MAX)
                {
                    goto error;
                }
                mysql_free_result(result);
                result = NULL;
                ret = snprintf(query, sizeof(query),
                               "update local_files set link_num = link_num - 1 where id = %ld and link_num > 0", localId);
                if(ret < 0 || ret >= (int)sizeof(query) || mysql_query(conn, query) != 0 ||
                   mysql_affected_rows(conn) != 1)
                {
                    goto error;
                }
            }
            else
            {
                // 上传未完成时可能只有虚拟文件记录
                mysql_free_result(result);
                result = NULL;
            }
        }
        ret = snprintf(query, sizeof(query),
                       "delete from virtual_file where id = %d and owner_id = %d", node->id, user_id);
        if(ret < 0 || ret >= (int)sizeof(query) || mysql_query(conn, query) != 0 ||
           mysql_affected_rows(conn) != 1)
        {
            goto error;
        }
    }
    if(mysql_commit(conn) != 0)
    {
        goto error;
    }
    free(nodes);
    mysql_close(conn);
    return 0;

error:
    if(result != NULL)
    {
        mysql_free_result(result);
    }
    if(conn != NULL)
    {
        if(inTransaction)
        {
            mysql_rollback(conn);
        }
        mysql_close(conn);
    }
    free(nodes);
    return -1;
}

// 获得父结点的id
int get_parent_id(int id, int *pparent_code)
{
    MYSQL *conn = NULL;
    MYSQL_RES *result = NULL;
    int ret = mysqlconnect(&conn);
    if(ret != 0)
    {
        goto error;
    }
    ret = mysql_query(conn, "set names 'utf8'");
    if(ret != 0)
    {
        fprintf(stderr, "Database query failed.\n");
        goto error;
    }
    char query[1024] = {0};
    ret = snprintf(query, sizeof(query), "select parent_id from virtual_file where id = %d", id);
    if(ret < 0 || ret >= (int)sizeof(query))
    {
        goto error;
    }

    ret = do_query(query, &conn);
    if(ret != 0)
    {
        goto error;
    }
    result = mysql_store_result(conn);
    if(result == NULL)
    {
        fprintf(stderr, "Database query failed.\n");
        goto error;
    }
    int rows = mysql_num_rows(result);
    
    if(rows == 0)// 文件不存在返回1
    {
        mysql_free_result(result);
        result = NULL;
        mysql_close(conn);
        return 1;
    }
    MYSQL_ROW row;
    if((row = mysql_fetch_row(result)) == NULL)
    {
        if(mysql_errno(conn) != 0)
        {
            goto error;
        }
        mysql_free_result(result);
        mysql_close(conn);
        return 1;
    }
    if(row[0] == NULL)
    {
        goto error;
    }
    char *end;
    errno = 0;
    long value = strtol(row[0], &end, 10);
    if(errno != 0 || end == row[0] || *end != '\0' || value < 0 || value > INT_MAX)
    {
        goto error;
    }
    *pparent_code = value;
    mysql_free_result(result);
    result = NULL;
    mysql_close(conn);
    return 0;

error:
    if(result != NULL)
    {
        mysql_free_result(result);
    }
    if(conn != NULL)
    {
        mysql_close(conn);
    }
    return -1;
}
