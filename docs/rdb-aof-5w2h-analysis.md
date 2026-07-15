# Redis RDB/AOF 持久化：5W2H 分析

> 建议先阅读 [00-introduction.md](00-introduction.md)，了解 Redis 是什么、应用场景、pthread 解决的问题、学习动机与目标。

面向刚入门 Redis 的读者。先把最容易混淆的一点说清楚：

> **RDB 快照和 AOF Rewrite 用的是 `fork()` 子进程，不是临时创建的 pthread 线程。**  
> **真正用 pthread 的是 BIO 后台线程**（负责 AOF fsync、慢速 close、lazyfree 等）。

持久化里的「后台」其实有**两套机制**，后面所有 Why/What/How 都围绕这一点展开。

---

## 术语表（阅读前先扫一眼）

| 术语 / 缩写 | 中文含义 | 本文中的用法 |
|-------------|----------|--------------|
| **5W2H** | Why / What / When / Where / Who / Whom / How | 下文分析框架：从动机到实现一路拆开 |
| **RDB** | Redis Database（快照文件） | 内存数据的二进制快照；由 fork 子进程执行 BGSAVE 落盘 |
| **AOF** | Append Only File（追加日志） | 把写命令追加到日志文件，用于重启恢复 |
| **BGSAVE** | Background Save | 后台触发 RDB 保存的命令 / 路径 |
| **BGREWRITEAOF** | Background Rewrite AOF | 后台触发 AOF 重写的命令 / 路径 |
| **Rewrite** | AOF 重写 | 把膨胀的命令历史压缩成「当前数据集」的等价 AOF |
| **BASE AOF** | 基准 AOF 文件 | Rewrite 完成后作为基线的 AOF |
| **INCR AOF** | 增量 AOF 文件 | Rewrite 期间父进程继续写入的增量日志 |
| **appendfsync** | AOF 刷盘策略配置 | `always` / `everysec` / `no`，决定何时 `fsync` |
| **BIO** | Background I/O（后台 I/O） | 启动时创建的常驻 pthread worker：fsync、close、lazyfree |
| **pthread** | POSIX 线程 | BIO 使用的线程模型（不是 fork 子进程） |
| **fork** | 创建子进程 | RDB / AOF Rewrite 用 OS `fork()` 拿内存快照 |
| **COW** | Copy-On-Write（写时复制） | fork 后父子共享页；谁先写谁拷贝，实现低成本快照 |
| **lazyfree** | 惰性释放 | 大对象 / 大库在 BIO 线程异步释放内存 |
| **bg_unlink** | 后台删文件 | 先 unlink 文件名，再把慢 `close` 丢给 BIO |
| **fsync** | 强制刷盘 | 把已 write 的数据推到持久化存储；everysec 时常走 BIO |
| **fd** | File Descriptor（文件描述符） | 打开文件的句柄；AOF 有 `aof_fd` 等 |
| **waitpid** | 等待子进程结束 | 父进程回收 fork 子进程并读退出状态 |
| **QPS** | Queries Per Second | 每秒请求数；主线程阻塞时延迟会飙升 |

---

## 一、先建立一张「地图」

```mermaid
flowchart LR
    subgraph Why [为什么要后台做]
        W1["磁盘 I/O 慢"]
        W2["不能阻塞主线程处理命令"]
    end

    subgraph What [两套后台机制]
        F["fork 子进程\nRDB / AOF Rewrite"]
        B["BIO pthread 线程\nfsync / close / lazyfree"]
    end

    subgraph How [怎么协作]
        H1["子进程：重活、快照"]
        H2["BIO：慢 I/O offload"]
        H3["主线程：命令 + 协调回收"]
    end

    Why --> What --> How
```

| 机制 | 创建方式 | 生命周期 | 典型任务 |
|------|----------|----------|----------|
| **fork 子进程** | `redisFork()` | 临时，干完就 exit | 写 RDB、AOF Rewrite |
| **BIO 线程** | `bioInit()` → `pthread_create` × 3 | 进程启动后常驻 | AOF fsync、文件 close、lazyfree |

### 读图时常见疑问：fork 子进程究竟是什么关系？

地图里写了「fork 子进程」和「主线程」，很容易默认成「主线程的一个子线程」。下面三个问题按阅读顺序澄清。

#### Q：子进程与主线程是什么关系？

**不是线程父子，而是进程父子。** 主线程只是**父进程**里负责发号施令和最后 `waitpid` 回收的那条执行流。

| 对比 | BIO 线程 | fork 子进程 |
|------|----------|-------------|
| 与主线程关系 | **同进程**内的兄弟线程，共享地址空间 | **父子进程**，各自独立 PID |
| 内存 | 真共享，一边改另一边看得见 | fork 瞬间逻辑视图相同；靠 **COW** 共享物理页，谁写谁拷贝 |
| 生命周期 | 启动常驻 | 干完 `_exit`，父进程 `waitpid` 回收 |
| 协作方式 | 任务队列 + 条件变量 / pipe | 子进程写临时文件；父进程看退出码做 rename / 清理 |

```mermaid
sequenceDiagram
    participant Main as 父进程主线程
    participant Child as fork子进程
    participant Disk as 磁盘

    Main->>Main: redisFork()
    Note over Main,Child: 此刻父子内存视图相同<br/>之后靠 COW 分离

    par 父进程继续服务
        Main->>Main: 处理客户端命令
        Main->>Main: 正常写 AOF buffer 等
    and 子进程只干重活
        Child->>Disk: 遍历快照写 temp 文件
        Child->>Child: _exit
    end

    Main->>Main: serverCron 到 waitpid
    Main->>Disk: rename 或失败清理
    Main->>Main: resetChildState
```

两层概念不要混：

1. **进程层**：父进程 ↔ 子进程（`fork` 的直接产物）
2. **线程层**：父进程里还有主线程 + BIO 等 pthread；子进程里通常只继续跑调用了 `fork` 的那条路径，BIO 不会在子进程里继续跑业务

一句话：**BIO 是帮主线程干慢 I/O 的同事；fork 子进程是被派出写快照、写完就下班的临时工。**

#### Q：是每次有任务都 fork 再消除，还是单独常驻一个进程处理？

**是第一种：按次 fork，干完就退出。** 不是单独常驻一个 fork 出来的进程一直等任务。

```text
无任务：  只有父进程（主线程 + BIO 等），child_pid = -1

有任务：  BGSAVE / BGREWRITEAOF / …
            → redisFork()          // 新 PID
            → 子进程：改标题 → 写盘 → exitFromChild() / _exit()
            → 父进程：记下 child_pid，继续服务
            → serverCron → waitpid → handler → resetChildState()
            → 又变成「没有子进程」

下次任务：再 redisFork() 一次，又是一个新 PID
```

源码形态（RDB）：`redisFork(CHILD_TYPE_RDB) == 0` 进入子进程分支 → `rdbSave(...)` → 退出。AOF Rewrite 同理。子进程用 `_exit()`（经 `exitFromChild`），避免跑完整 `exit()` 清理误伤父进程还要用的资源。

| | fork 子进程 | BIO |
|--|-------------|-----|
| 创建次数 | **每次** BGSAVE / Rewrite 等才创建 | 启动时 `bioInit()` **一次**建 3 个 |
| 退出 | 任务结束即 `_exit` | 常驻到 Redis 进程退出 |
| 排队 | 同一时刻通常**最多一个**互斥类子进程 | 任务丢进队列，worker 反复取 |

#### Q：这种 fork 不会抬升 CPU 占用率吗？

**会，但要分清：抬升主要不是「fork 这一下」，而是子进程在干活的那段时间。** Redis 用短时脉冲换「主线程不被锁死」。

| 阶段 | 对 CPU 的影响 | 量级直觉 |
|------|----------------|----------|
| `fork()` 本身 | 主要是页表 / 元数据，**不会立刻拷贝整库内存** | 通常短促；大库仍可能有一次尖峰 |
| 子进程跑起来 | 遍历键空间、序列化、`write` 临时文件 | **这段最明显**：等于多出一个忙于写盘的进程 |
| COW 缺页 | 父进程在 BGSAVE 期间改内存页 → 整页拷贝 | CPU + 内存带宽一起涨；写越多越重 |
| 父进程主线程 | 仍在处理命令 | 自身 CPU 不一定更高，但整机争用会上去 |

常见现象：`BGSAVE` / `BGREWRITEAOF` 期间 `INFO` 里 `used_cpu_*_children` 起来；主线程延迟可能因争用略差，但仍远好于「主线程自己同步写整库」。

为什么还敢每次 fork：

- **间歇性**：任务结束子进程就退出，不是常驻打满一核
- **互斥**：同一时刻基本只允许一个这类子进程
- **换的是延迟**：重活挪出主事件循环
- **对比常态 fsync**：高频短任务走 BIO，就是怕每次都 fork 受不了

运维上可观察 `rdb_last_cow_size` 等 cow 指标；也可把子进程绑到非主业务核（`bgsave_cpulist` / `aof_rewrite_cpulist`），并把 save / auto-aof-rewrite 错峰。

**结论**：CPU 抬升是预期代价；目标不是「零 CPU」，而是别堵死响应命令的主线程，并把抬升限制成短时、可调度、互斥的脉冲。

---

## 二、5W2H 完整分析

### 1. Why — 为什么要这样设计？

#### 1.1 为什么要「后台」持久化？

Redis 是内存数据库，数据在 RAM 里。如果每次写盘都在主线程同步完成：

- `write()` / `fsync()` 可能阻塞**毫秒到秒级**
- 所有客户端命令都会被拖慢
- 高 QPS 下延迟会急剧上升

所以必须把**慢操作**挪出主线程的执行路径。

#### 1.2 为什么 RDB/AOF Rewrite 用 fork，而不是 pthread？

| 方案 | 问题 |
|------|------|
| 多线程共享内存写 RDB | 要对整个 DB 加锁，复杂且慢 |
| **fork + COW** | 子进程看到 fork 时刻的内存快照，遍历写文件时**几乎不用锁** |

fork 的核心价值：**用 OS 的 Copy-On-Write，以较低成本拿到一份「逻辑快照」**。

#### 1.3 为什么 AOF 常态 fsync 用 BIO 线程，而不是 fork？

- fsync 是**高频、短周期**操作（everysec 每秒一次）
- 为每次 fsync fork 一个进程，开销太大
- 一个**常驻 worker 线程** + 任务队列更合适

#### 1.4 为什么删旧 AOF 文件也要「后台 close」？

Linux 上 `unlink()` 只删文件名；文件内容要等**最后一个 fd close** 才真正释放。  
close 大文件可能很慢 → 交给 BIO 线程。

---

### 2. What — 涉及哪些概念和组件？

#### 2.1 角色

| 角色 | 是什么 | 职责 |
|------|--------|------|
| **主线程** | Redis 主事件循环 | 处理命令、AOF buffer write、触发 fork、提交 BIO 任务、回收子进程 |
| **fork 子进程** | `redis-rdb-bgsave` / `redis-aof-rewrite` | 遍历内存写 RDB 或 Rewrite AOF |
| **BIO worker 0** | pthread `bio_close_file` | 后台 close 文件（unlink 后的真正释放） |
| **BIO worker 1** | pthread `bio_aof` | AOF `fsync()`、close 旧 AOF fd |
| **BIO worker 2** | pthread `bio_lazy_free` | 大对象/大库 lazyfree |

#### 2.2 关键文件

| 文件 | 内容 |
|------|------|
| [src/bio.c](../src/bio.c) | BIO 线程创建、任务队列、fsync/close 执行 |
| [src/rdb.c](../src/rdb.c) | BGSAVE、`backgroundSaveDoneHandler` |
| [src/aof.c](../src/aof.c) | AOF 写入、Rewrite、`backgroundRewriteDoneHandler` |
| [src/server.c](../src/server.c) | `redisFork`、`checkChildrenDone`、`InitServerLast` |
| [src/replication.c](../src/replication.c) | `bg_unlink`（后台删文件） |

#### 2.3 关键状态变量

| 变量 | 含义 |
|------|------|
| `server.child_pid` | 当前 fork 子进程 PID，`-1` 表示没有 |
| `server.child_type` | `CHILD_TYPE_RDB` / `CHILD_TYPE_AOF` / `NONE` |
| `server.aof_fd` | 当前 INCR AOF 文件描述符 |
| `server.aof_buf` | 主线程 AOF 写缓冲 |
| `server.aof_bio_fsync_status` | BIO fsync 是否出错（atomic） |
| `server.lastbgsave_status` | 最近一次 BGSAVE 结果 |

#### 2.4 互斥规则

RDB、AOF Rewrite、Module fork **三者互斥**——同一时刻最多一个 `hasActiveChildProcess()`。

---

### 3. When — 什么时候创建？什么时候回收？

#### 3.1 BIO 线程

| 时机 | 动作 |
|------|------|
| **Redis 启动后** `InitServerLast()` | `bioInit()` 创建 3 个 pthread，**常驻到进程退出** |
| **每次 AOF everysec** | 主线程提交 fsync job，**不创建新线程** |
| **删旧 AOF / bg_unlink** | 主线程提交 close job |
| **进程 crash** | `bioKillThreads()` 强制结束（极少路径） |

#### 3.2 RDB 子进程

| 时机 | 动作 |
|------|------|
| `BGSAVE` 命令 | `rdbSaveBackground()` → fork |
| `save 900 1` 等条件满足 | `serverCron` 自动触发 |
| 主从复制 SYNC | 需要 RDB 时 fork |
| **子进程写完** | `_exit(0/1)` |
| **父进程** `serverCron` | `checkChildrenDone()` → `waitpid` → handler → `resetChildState()` |

#### 3.3 AOF Rewrite 子进程

| 时机 | 动作 |
|------|------|
| `BGREWRITEAOF` | `rewriteAppendOnlyFileBackground()` → fork |
| AOF 文件过大自动 rewrite | `serverCron` 调度 |
| 首次 `appendonly yes` | `startAppendOnly()` 触发 |
| **Rewrite 前** | `bioDrainWorker(BIO_AOF_FSYNC)` 等旧 fsync 全部完成 |
| **子进程完成** | `_exit()` |
| **父进程** | `backgroundRewriteDoneHandler()` → rename → 删旧文件 |

#### 3.4 主动取消

| 场景 | 方式 |
|------|------|
| 取消 BGSAVE | `killRDBChild()` → `SIGUSR1` |
| 取消 AOF Rewrite | `killAppendOnlyChild()` → `SIGUSR1` + 同步 `waitpid` |

`SIGUSR1` 是「白名单信号」——表示主动取消，不算持久化失败。

---

### 4. Where — 逻辑在代码哪里？

#### 4.1 启动入口

```
main()
  → initServer()
  → ... 加载配置、模块 ...
  → InitServerLast()
       → bioInit()          // BIO 线程在这里创建
       → initThreadedIO()   // IO 线程（与持久化无直接关系）
```

#### 4.2 RDB 路径

```
BGSAVE / serverCron
  → rdbSaveBackground()          [rdb.c]
       → redisFork(CHILD_TYPE_RDB) [server.c]
            子进程: rdbSave() → exitFromChild()
            父进程: 记录 child_pid

serverCron (每 100ms)
  → checkChildrenDone()          [server.c]
       → backgroundSaveDoneHandler() [rdb.c]
       → resetChildState()
```

#### 4.3 AOF 常态写入路径

```
命令执行 → 追加到 aof_buf
  → flushAppendOnlyFile()        [aof.c]
       → write(aof_fd)            // 主线程
       → aof_background_fsync()   // everysec
            → bioCreateFsyncJob() [bio.c]
                 → BIO worker1 执行 fsync
```

#### 4.4 AOF Rewrite 路径

```
BGREWRITEAOF / serverCron
  → rewriteAppendOnlyFileBackground() [aof.c]
       → flushAppendOnlyFile()
       → openNewIncrAofForAppend()    // 父进程开新 INCR AOF
       → bioDrainWorker(BIO_AOF_FSYNC)
       → redisFork(CHILD_TYPE_AOF)
            子进程: rewriteAppendOnlyFile(temp) → exit
            父进程: 继续写新 INCR AOF

  → checkChildrenDone()
       → backgroundRewriteDoneHandler() [aof.c]
            rename temp → BASE
            aofDelHistoryFiles() → bg_unlink() → bioCreateCloseJob()
```

---

### 5. Who — 谁在做？谁负责什么？

```mermaid
sequenceDiagram
    participant Client as 客户端
    participant Main as 主线程
    participant Child as fork子进程
    participant BIO as BIO线程

    Note over Main,BIO: 启动时：bioInit() 创建 BIO（常驻）

    Client->>Main: SET key value
    Main->>Main: 改内存 + 追加 aof_buf
    Main->>Main: write(aof_fd)
    Main->>BIO: bioCreateFsyncJob (everysec)
    BIO->>BIO: fsync()

    Client->>Main: BGSAVE
    Main->>Child: redisFork()
    Child->>Child: rdbSave() 写 temp.rdb
    Child->>Child: _exit()
    Main->>Main: waitpid → handler → rename/清理

    Client->>Main: BGREWRITEAOF
    Main->>Main: openNewIncrAofForAppend()
    Main->>Child: redisFork()
    Child->>Child: rewriteAppendOnlyFile(temp)
    Child->>Child: _exit()
    Main->>Main: rename + 更新 manifest
    Main->>BIO: bg_unlink → bioCreateCloseJob
```

**职责边界（新人必记）**：

| 谁 | 做什么 | 不做什么 |
|----|--------|----------|
| 主线程 | 命令、AOF write、触发 fork、提交 BIO、waitpid | 不做 fsync（everysec）、不做慢 close |
| fork 子进程 | 读内存快照写 RDB/Rewrite AOF | 不处理客户端、不改共享数据结构 |
| BIO 线程 | fsync、close、lazyfree | 不执行 Redis 命令、不 fork |

---

### 6. Whom — 影响谁？为谁服务？

| 设计决策 | 受益者 | 影响 |
|----------|--------|------|
| fork 写 RDB | 所有需要快照的场景（重启恢复、复制） | 父进程内存可能 COW 膨胀 |
| BIO 异步 fsync | 使用 `appendfsync everysec` 的用户 | 最多丢 1 秒数据 |
| AOF Rewrite | 磁盘空间、重启加载速度 | Rewrite 期间 fork 有短暂开销 |
| `bg_unlink` | 主线程延迟 | 磁盘释放稍延后 |
| 子进程互斥 | 系统稳定性 | 不能同时 BGSAVE + BGREWRITEAOF |

**对不同角色的意义**：

- **业务开发者**：默认 everysec，知道「最多丢 1 秒」
- **运维**：关注 `lastbgsave_status`、AOF rewrite 是否成功、磁盘空间
- **源码阅读者**：先分清 fork 路径和 BIO 路径，再读 handler

---

### 7. How — 具体怎么实现？

#### 7.1 BIO 线程怎么创建？

`bioInit()` 做四件事：

1. 每个 worker：`pthread_mutex` + `pthread_cond` + 任务队列
2. 创建 `job_comp_pipe`，BIO 完成后唤醒主线程事件循环
3. 设置栈 4MB，`pthread_create` 启动 3 个 worker
4. worker 循环：`cond_wait` 等任务 → 执行 → 继续等

任务提交（主线程）：`lock → 入队 → cond_signal → unlock`

#### 7.2 fork 子进程怎么创建？

`redisFork(purpose)` 统一封装：

**父进程**：记录 `child_pid`、`child_type`；打开 `child_info_pipe`；统计 fork 耗时

**子进程**：设置 signal handler、OOM score；关闭不需要的 fd；执行具体任务后 `exitFromChild()` → `_exit()`

#### 7.3 资源怎么回收？

**fork 子进程**：

```
子进程 _exit()
  → 父进程 checkChildrenDone()
       → waitpid(-1, WNOHANG)
       → backgroundXxxDoneHandler()
       → resetChildState()
       → closeChildInfoPipe()
```

**临时文件**：

| 场景 | 处理方式 |
|------|----------|
| RDB 成功 | rename `temp-<pid>.rdb` → 正式 RDB |
| RDB 失败/被杀 | `rdbRemoveTempFile()` / `bg_unlink` |
| AOF Rewrite 成功 | rename `temp-rewriteaof-bg-<pid>.aof` → 新 BASE |
| AOF Rewrite 失败 | `aofRemoveTempFile()` |

**旧 AOF 文件**：

```
aofDelHistoryFiles()
  → bg_unlink(filepath)
       → open() + unlink()        // 主线程：删名字
       → bioCreateCloseJob(fd)    // BIO：close 释放 inode
```

---

## 三、两条主线串讲

### 主线 A：BGSAVE（RDB）

```
1. 触发：BGSAVE / 定时 save / 复制需要
2. 检查：hasActiveChildProcess() 必须为 false
3. fork：
   - 子：rdbSave() 写 temp-<pid>.rdb
   - 父：记录 child_pid，继续服务
4. 回收：
   - waitpid → backgroundSaveDoneHandler
   - 成功：更新 lastsave
   - 失败：删 temp 文件
   - resetChildState()
```

### 主线 B：AOF 日常 + Rewrite

**日常写入**：

```
命令 → aof_buf → flushAppendOnlyFile()
  → write() 主线程
  → everysec: bioCreateFsyncJob → BIO fsync
```

**Rewrite**：

```
1. flush 现有 buffer
2. openNewIncrAofForAppend()   // 父进程换新 INCR 继续写
3. bioDrainWorker(AOF_FSYNC)   // 等旧 fsync 完成
4. fork：
   - 子：rewriteAppendOnlyFile(temp)  // 写 BASE
   - 父：新命令写新 INCR
5. 回收：
   - rename temp → BASE
   - 旧 INCR → HISTORY
   - 更新 manifest
   - bg_unlink 删 HISTORY → BIO close
```

---

## 四、新人常见问题（FAQ）

**Q1：持久化用的是线程还是进程？**  
A：Rewrite/RDB 用 **fork 子进程**；AOF fsync 和慢 close 用 **BIO pthread 线程**。

**Q2：BIO 线程是每次 BGSAVE 才创建吗？**  
A：不是。启动时 `bioInit()` 创建一次，常驻运行。

**Q3：`appendfsync always` 会用 BIO 吗？**  
A：不会。fsync 在主线程同步执行，最安全也最慢。

**Q4：Rewrite 时新命令写哪里？**  
A：父进程写**新的 INCR AOF**；子进程写**临时 BASE 文件**，互不干扰。

**Q5：怎么知道 BGSAVE 是否成功？**  
A：`INFO persistence` 看 `rdb_last_bgsave_status`；或日志 `Background saving terminated with success`。

**Q6：SIGUSR1 是干什么的？**  
A：主动取消子进程的信号，handler 里不算错误。

**Q7：fork 子进程和主线程是什么关系？**  
A：是**父子进程**，不是主线程的子线程。主线程在父进程里发号施令并 `waitpid`；子进程写临时文件后 `_exit`。详细对照见「一、地图」下「读图时常见疑问」。

**Q8：fork 是每次任务新建再消除，还是常驻一个子进程？**  
A：**每次** BGSAVE / BGREWRITEAOF 等才 `redisFork()` 一次，干完就退出；下次再 fork 出新 PID。常驻的是 BIO，不是 fork 子进程。

**Q9：频繁 / 每次 fork 会不会抬高 CPU？**  
A：会。尖峰主要来自**子进程遍历写盘**以及期间的 **COW 缺页**，不是单单 `fork()` 调用本身。这是用短时 CPU 脉冲换主线程不被锁死；可错峰、看 cow 指标、给子进程绑核。

---

## 五、5W2H 速查总表

| 维度 | RDB (BGSAVE) | AOF 日常写入 | AOF Rewrite |
|------|--------------|--------------|-------------|
| **Why** | 快照恢复、复制 | 记录每条写命令 | 压缩 AOF、加快加载 |
| **What** | fork 子进程写 RDB | 主线程 write + BIO fsync | fork 子进程写 BASE |
| **When 创建** | 命令/定时/复制 | 每条写命令 | BGREWRITEAOF/自动 |
| **When 回收** | 子进程 exit + waitpid | BIO job 执行完 | exit + rename + bg_unlink |
| **Where** | rdb.c | aof.c + bio.c | aof.c |
| **Who 执行** | fork 子进程 | 主线程 + BIO | fork 子进程 + 主线程 |
| **Whom 影响** | 重启恢复、slave 同步 | 数据 durability | 磁盘占用、加载速度 |
| **How** | fork → rdbSave → handler | buffer → write → bio fsync | fork → rewrite → rename |

---

## 六、一句话记忆

> **重活用 fork（RDB/Rewrite），慢 I/O 用 BIO（fsync/close），主线程只负责指挥和回收。**
