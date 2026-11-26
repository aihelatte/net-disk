# 新手友好 · 面试可讲 C++17 代码风格规范

本规范适用于网盘的客户端和服务端。整理已有代码时，以当前实现为基准，保持命令行为、返回码、收发顺序、SQL 执行顺序、事务边界和锁的作用域一致。

## 1. 用具名函数展开嵌套写法

辅助类型和函数集中放在实现文件顶部的匿名命名空间中，或使用文件作用域的 `static` 函数。函数体内避免定义局部类型、复杂 Lambda 和立即执行 Lambda。

**反例**

```cpp
static const auto commands = [] {
    CommandMap table;
    table.emplace(CD, std::make_unique<CdCommand>());
    return table;
}();
```

**正例**

```cpp
CommandMap buildCommands()
{
    CommandMap table;
    table.emplace(CD, std::make_unique<CdCommand>());
    return table;
}

const CommandMap &commands()
{
    static const CommandMap table = buildCommands();
    return table;
}
```

## 2. 给 C 资源配具名删除器

用不保存状态的结构体封装释放函数。常见的标准库实现可对空删除器使用空基类优化或等效布局，省去单独存储函数指针的空间。需要根据 C API 的关闭结果决定返回值时，显式通过 `release()` 移交一次，然后检查关闭结果。

**反例**

```cpp
std::unique_ptr<MYSQL, decltype(&mysql_close)> conn(mysql_init(nullptr), mysql_close);
```

**正例**

```cpp
struct MysqlConnDeleter
{
    void operator()(MYSQL *conn) const
    {
        mysql_close(conn);
    }
};

using MysqlConnPtr = std::unique_ptr<MYSQL, MysqlConnDeleter>;
MysqlConnPtr conn(mysql_init(nullptr));
```

## 3. 让类型在阅读位置可见

函数签名、普通变量和一般调用结果写出类型。迭代器及右侧已直接给出类型的构造表达式可以用 `auto`。

**反例**

```cpp
auto conn = open_connection();
auto size = readFileSize();
```

**正例**

```cpp
MysqlConnPtr conn = open_connection();
int64_t size = readFileSize();
const auto it = commands.find(state);
auto entry = std::make_unique<Entry>(std::move(socket));
```

## 4. 给控制流加大括号，按顺序检查失败

`if`、`else`、`for`、`while` 的循环体和分支体即使只有一行也加大括号。块级左括号另起一行。连续的 I/O、数据库和状态更新逐步检查，拆分旧条件时保留短路顺序和错误码。三目表达式只用于一眼可读的取值。

**反例**

```cpp
if (!valid || sendRequest() < 0 || readReply() < 0) return -1;
```

**正例**

```cpp
if (!valid)
{
    return -1;
}
if (sendRequest() < 0)
{
    return -1;
}
int result = readReply();
if (result < 0)
{
    return -1;
}
return 0;
```

## 5. Lambda 只保留简短、就地可读的表达式

条件变量谓词适合放在等待处。超过三行的复用步骤用具名函数，并用参数和返回值表达依赖。

**反例**

```cpp
auto selectNodes = [&](int length) {
    if (length < 0)
    {
        return -1;
    }
    return queryAndAppend(conn, query, nodes);
};
```

**正例**

```cpp
int result = selectDeleteNodes(conn, query, length, nodes);
cond.wait(lock, [this] { return exitFlag || !taskQueue.empty(); });
```

## 6. 中文注释解释职责、结果与原因

核心类前说明职责和持有的资源。关键函数前用一到两句话说明输入、步骤和返回值。对 `std::move`、`release()`、显式解锁与加锁、时间轮 `splice`、多态派发、事务提交和回滚，写清为何在这个位置执行。

**反例**

```cpp
// 移动 fd。
task.netFd = std::move(fd);
```

**正例**

```cpp
// 队列位置已分配成功，再移交套接字。分配失败时原连接仍负责关闭。
task.netFd = std::move(fd);
```

## 7. 用常规写法保持简洁，清理冗余

在保持语义和调用顺序的前提下，删除注释掉的调试语句、废弃的死代码和无用声明。保留解释业务规则或设计原因的注释。同一文件内完全重复的小段逻辑优先复用已有的轻量函数，例如短命令的状态码回包。避免只为省几行代码引入晦涩的宏、模板或额外抽象。

**反例**

```cpp
// printf("check = %d\n", check);
if (wire_queue_int(connection, check) != sizeof(check))
{
    return -1;
}
return 0;
```

**正例**

```cpp
return queueCommandStatus(connection, check);
```
