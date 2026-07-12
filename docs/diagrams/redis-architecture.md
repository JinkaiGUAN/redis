# Redis 架构图（PlantUML 源文件）

与 [00-introduction.md](../00-introduction.md) 第三节中的 Mermaid 图对应。  
将下方代码复制到 [PlantUML 在线编辑器](https://www.plantuml.com/plantuml) 或 IDE PlantUML 插件中渲染。

---

## 1. 逻辑分层架构

```plantuml
@startuml redis-layered-architecture
title Redis 逻辑分层架构

package "客户端层" {
  [redis-cli]
  [业务应用]
  [redis-benchmark]
}

package "网络层" {
  [connection.c]
  [networking.c]
  [resp_parser.c]
}

package "事件调度层" {
  [ae.c 事件循环]
  [iothread.c IO线程]
  [eventnotifier.c]
}

package "命令执行层（主线程）" {
  [commands.c 命令表]
  [db.c 键空间]
  [t_string / t_hash / t_list ...]
}

package "支撑子系统" {
  [replication.c 复制]
  [rdb.c / aof.c 持久化]
  [bio.c 后台I/O]
  [zmalloc / object 内存]
  [eval / module 脚本]
}

database "内存数据库" as mem
database "RDB 文件" as rdb
database "AOF 文件" as aof

[业务应用] --> [connection.c]
[redis-cli] --> [connection.c]
[connection.c] --> [networking.c]
[networking.c] --> [resp_parser.c]
[resp_parser.c] --> [ae.c 事件循环]
[iothread.c IO线程] --> [ae.c 事件循环]
[ae.c 事件循环] --> [commands.c 命令表]
[commands.c 命令表] --> [db.c 键空间]
[db.c 键空间] --> [t_string / t_hash / t_list ...]
[t_string / t_hash / t_list ...] --> mem
[commands.c 命令表] --> [rdb.c / aof.c 持久化]
[rdb.c / aof.c 持久化] --> [bio.c 后台I/O]
[rdb.c / aof.c 持久化] --> rdb
[bio.c 后台I/O] --> aof

@enduml
```

---

## 2. 运行时进程/线程模型

```plantuml
@startuml redis-runtime-threads
title Redis 运行时进程/线程模型

rectangle "redis-server 进程" {
  rectangle "主线程 (io thread 0)" as main {
    [aeEventLoop]
    [serverCron]
    [命令执行]
  }

  rectangle "IO 线程池（可选）" as iopool {
    [IOThread 1]
    [IOThread N]
  }

  rectangle "BIO 线程 x3（常驻）" as bio {
    [worker0 close]
    [worker1 fsync]
    [worker2 lazyfree]
  }
}

rectangle "fork 子进程（临时）" as child {
  [redis-rdb-bgsave]
  [redis-aof-rewrite]
}

actor 客户端 as client
client --> iopool
iopool --> main : pending_clients
main --> bio : bioSubmitJob
bio --> main : pipe 唤醒
main --> child : redisFork
child --> main : exit + waitpid

@enduml
```

---

## 3. 启动流程

```plantuml
@startuml redis-startup-sequence
title Redis 启动流程

participant "main()" as main
participant "initServerConfig()" as config
participant "initServer()" as init
participant "loadDataFromDisk()" as load
participant "InitServerLast()" as last
participant "aeMain()" as loop

main -> config : 解析 redis.conf / 命令行
main -> init : 创建事件循环、数据库、客户端列表
note right of init : server.main_thread_id = pthread_self()
main -> load : 加载 RDB 或 AOF 到内存
main -> last : bioInit() 创建 3 个 BIO 线程
note right of last : initThreadedIO() 创建 IO 线程
main -> loop : 进入事件循环，开始接受连接

@enduml
```

---

## 4. 持久化子系统

```plantuml
@startuml redis-persistence-subsystem
title 持久化子系统在整体架构中的位置

rectangle "命令路径（主线程）" {
  [命令修改内存]
}

rectangle "AOF 路径" {
  [aof_buf]
  [write 主线程]
  [BIO fsync]
}

rectangle "RDB 路径" {
  [redisFork]
  [rdbSave 子进程]
}

rectangle "AOF Rewrite 路径" {
  [redisFork]
  [rewrite 子进程]
  [rename + manifest]
  [bg_unlink + BIO close]
}

[命令修改内存] --> [aof_buf]
[aof_buf] --> [write 主线程]
[write 主线程] --> [BIO fsync]
[命令修改内存] --> [redisFork] : BGSAVE
[redisFork] --> [rdbSave 子进程]
[命令修改内存] --> [redisFork] : BGREWRITEAOF
[rewrite 子进程] --> [rename + manifest]
[rename + manifest] --> [bg_unlink + BIO close]

@enduml
```
