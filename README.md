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
```

服务端数据库连接读取以下环境变量：

```bash
export NET_DISK_DB_HOST=localhost
export NET_DISK_DB_USER=net_disk_app
export NET_DISK_DB_PASSWORD='服务器本地密码'
export NET_DISK_DB_NAME=net_disk
```

## 当前状态

当前提交用于保存经过敏感信息清理的原 C 项目基线。项目仍缺少 `myhead.h`、完整的 l8w8jwt 依赖说明和 MySQL 建表脚本，因此尚未宣称可以构建或运行。后续将在实验室服务器上补齐环境并记录验证结果。
