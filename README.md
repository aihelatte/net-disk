# net-disk

Linux C 语言网盘课程项目的原版修复与复习仓库。

第一阶段仅恢复和修复原 C 版本，使其能够在实验室 Linux 服务器上构建、运行和测试；暂不进行 C++ 重构，也不引入 Redis、HTTP、Nginx、Docker 或消息队列等后续技术栈。

## 目录

- `code/client/`：客户端、控制连接、命令处理及上传下载。
- `code/server/`：服务端、epoll、线程池、MySQL、虚拟目录及文件传输。
- `docs/`：Bug 基线、修复计划和后续技术路线。

演示截图、`网盘项目示例/`、大型测试文件、编译产物和运行数据保留在本地，不纳入仓库。

## 本地配置

真实服务器地址和数据库密码不进入 Git。首次运行前，在对应运行机器上复制配置模板：

```bash
cp code/server/config/server.conf.example code/server/config/server.conf
cp code/client/config/client.conf.example code/client/config/client.conf
cp code/server/config/database.env.example code/server/config/database.env
```

将 `code/server/config/database.env` 的权限设为 `600`，填入服务器本地密码后加载：

```bash
chmod 600 code/server/config/database.env
source code/server/config/database.env
```

服务端会从该文件导出的环境变量中读取数据库地址、账号、密码和库名。真实的 `database.env` 仅保留在服务器，由 `.gitignore` 排除。

## Linux 构建依赖

客户端和服务端共用项目内公共头文件 `code/include/myhead.h`。服务端还需要兼容 MySQL C API 的开发头文件和链接库；在 CentOS 7 上由 `mariadb-devel` 提供。

Token 功能使用课程配套的 `l8w8jwt-2.0.0-linux-x86_64.tar.gz`。该第三方压缩包和预编译静态库不纳入 Git，应在 Linux 服务器上解压为：

```text
code/l8w8jwt/
├── include/l8w8jwt/
└── bin/release/
```

两个 Makefile 会从该目录读取 l8w8jwt 头文件和静态库，不需要把 `.a` 文件复制到系统 `/usr/lib`。

## 当前状态

当前分支正在恢复 Linux 构建和运行基线。项目尚未完成服务器编译、数据库初始化和端到端验证，因此尚未宣称可以构建或运行。
