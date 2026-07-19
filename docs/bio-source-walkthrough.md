# BIO 源码导读：从 Redis 初始化到后台 I/O 线程

> 本文是 [rdb-aof-learning-roadmap.md](rdb-aof-learning-roadmap.md) **阶段 1** 的详细展开。  
> 建议配合 [pthread-vs-embedded.md](pthread-vs-embedded.md) 第二节阅读（mutex+cond 与 `taskSpawn`/`semTake` 对照）。  
> **导读注释已同步写入源码**：`src/bio.c`、`src/bio.h`、`src/server.c`、`src/aof.c`、`src/replication.c`、`src/lazyfree.c`。  
> 学习用注释统一为 **中文 `【导读】` 前缀**，与 Redis 原有英文注释区分。

---

## 一、导读总览

### 1.1 五阶段阅读路线

```mermaid
flowchart TD
    P0["阶段0_预备\nbio.h + DESIGN"]
    P1["阶段1_启动链\nmain → bioInit"]
    P2["阶段2_BIO内核\nSubmit → Worker"]
    P3["阶段3_完成通知\npipe → ae"]
    P4["阶段4_业务调用\naof / bg_unlink"]
    P5["阶段5_动手验证\nCLion/gdb"]

    P0 --> P1 --> P2 --> P3 --> P4 --> P5
```

### 1.2 全局关系图（时序优先）

> **读图要点**：时间轴从上到下。整条链都由**主线程**编排：先创建 BIO，再进 `aeMain`；AOF 入口是主线程循环里的业务调用，不是另起线程。

#### 1.2.1 启动期：主线程创建 BIO，再进入事件循环

```mermaid
sequenceDiagram
    autonumber
    participant Main as 主线程
    participant Init as initServer
    participant Last as InitServerLast
    participant BIO as bioInit
    participant W0 as BIO_worker0
    participant W1 as BIO_worker1_aof
    participant W2 as BIO_worker2
    participant AE as aeMain

    Main->>Init: 创建 server.el / DB / main_thread_id
    Note over Main,Init: 此时尚无 BIO 线程
    Main->>Last: 模块与 listener 就绪后
    Last->>BIO: bioInit()
    BIO->>W0: pthread_create
    BIO->>W1: pthread_create
    BIO->>W2: pthread_create
    Note over W0,W2: 三个常驻 worker 阻塞在 cond_wait
    Main->>Main: loadDataFromDisk（7.4：BIO 已存在）
    Main->>AE: 进入事件循环（此后运行期）
```

#### 1.2.2 运行期：主线程驱动 AOF，再把 fsync 交给 BIO

以 `appendfsync everysec` 为例（时间关系最清晰）：

```mermaid
sequenceDiagram
    autonumber
    participant Cli as 客户端
    participant Main as 主线程_aeMain
    participant Sleep as beforeSleep
    participant AOF as flushAppendOnlyFile
    participant Bg as aof_background_fsync
    participant Sub as bioSubmitJob
    participant W1 as BIO_worker1_aof

    Cli->>Main: SET key value
    Main->>Main: 改内存 + 追加 aof_buf
    Note over Main: 事件处理结束后准备 sleep
    Main->>Sleep: beforeSleep()
    Sleep->>AOF: flushAppendOnlyFile(0)
    AOF->>AOF: write(aof_fd)  【主线程完成写盘缓冲】
    alt 距上次 fsync ≥ 1s 且无进行中的 fsync
        AOF->>Bg: aof_background_fsync(fd)
        Bg->>Sub: bioCreateFsyncJob → bioSubmitJob
        Sub->>W1: lock → 入队 → cond_signal
        Note over Main,Sleep: 主线程立即返回，不阻塞在 fsync
        W1->>W1: redis_fsync(fd)  【慢 I/O 在 worker】
        W1->>W1: 更新 fsynced_reploff_pending
    else always 策略或其他路径
        AOF->>AOF: 主线程直接 redis_fsync（不经 BIO）
    end
    Main->>Main: epoll_wait 等待下一轮事件
```

#### 1.2.3 结构总览（谁调用谁，对照上面时序）

```mermaid
flowchart LR
    subgraph boot [启动期]
        main["main"] --> initSrv["initServer"]
        initSrv --> initLast["InitServerLast"]
        initLast --> bioInit["bioInit"]
        bioInit -->|"pthread_create×3"| worker["BIO workers"]
        initLast --> aeMain["aeMain"]
    end

    subgraph runtime [运行期_主线程]
        aeMain --> beforeSleep["beforeSleep"]
        beforeSleep --> flush["flushAppendOnlyFile"]
        flush -->|"everysec"| fsyncJob["bioCreateFsyncJob"]
        fsyncJob -->|"cond_signal"| worker
        worker -->|"pipe 可读"| pipeR["bioPipeReadJobCompList"]
        aeMain --> pipeR
    end
```

对照：1.2.1 / 1.2.2 看**先后顺序**；1.2.3 看**静态调用边**。AOF 挂在 `aeMain → beforeSleep` 下，与「主线程负责启动与调度」一致。

更多「为何曾漏画这条边 / ae 与 comp list 是什么」见 [十、常见疑问（FAQ）](#十常见疑问faq)。

### 1.3 精简阅读清单（可打印勾选）

```
[ ] bio.h                          — API 与 job 类型
[ ] bio.c:1-36                     — DESIGN 注释
[ ] bio.c:58-114                   — 路由表 + bio_job 结构
[ ] server.c:6909                  — main()
[ ] server.c:2592, 2612, 2658     — initServer：主线程ID、ae 事件循环
[ ] server.c:7181-7198             — InitServerLast 调用时机
[ ] server.c:2881                  — InitServerLast → bioInit
[ ] bio.c:124                      — bioInit（重点）
[ ] bio.c:178                      — bioSubmitJob
[ ] bio.c:244                      — bioCreateFsyncJob
[ ] bio.c:253                      — bioProcessBackgroundJobs（重点）
[ ] bio.c:410                      — bioPipeReadJobCompList
[ ] aof.c:905, 1045                — 业务入口
[ ] replication.c:78               — bg_unlink
```

---

## 二、阶段 0：预备

### 2.1 [bio.h](../src/bio.h) — 对外 API 一览

```c
/* 【导读】BIO 对外只暴露「创建 job」和「管理 worker」接口，不暴露 bio_job 内部结构 */

typedef enum bio_worker_t {
    BIO_WORKER_CLOSE_FILE = 0,   // worker0：后台 close 文件
    BIO_WORKER_AOF_FSYNC,        // worker1：AOF fsync / close AOF
    BIO_WORKER_LAZY_FREE,        // worker2：lazyfree 大对象
    BIO_WORKER_NUM
} bio_worker_t;

typedef enum bio_job_type_t {
    BIO_CLOSE_FILE = 0,
    BIO_AOF_FSYNC,
    BIO_LAZY_FREE,
    BIO_CLOSE_AOF,
    BIO_COMP_RQ_CLOSE_FILE,      // completion 请求：完成后通知主线程
    BIO_COMP_RQ_AOF_FSYNC,
    BIO_COMP_RQ_LAZY_FREE,
    BIO_NUM_OPS
} bio_job_type_t;

void bioInit(void);                              // 启动时创建 3 个线程
void bioCreateFsyncJob(...);                    // 持久化：AOF fsync
void bioCreateCloseJob(...);                     // 持久化：后台 close（bg_unlink）
void bioCreateLazyFreeJob(...);                  // 大 key 删除
void bioDrainWorker(int job_type);               // 阻塞直到某类 job 全部完成
```

**自检**：3 个 worker 与 7 种 job type 的对应关系，在 [bio.c:58–66](../src/bio.c) 的 `bio_job_to_worker[]` 查表。

### 2.2 DESIGN 注释（[bio.c:15–36](../src/bio.c)）要点

| 设计点 | 含义 |
|--------|------|
| 每种 job 类型路由到固定 worker | 同类型 job **FIFO** 串行执行，避免竞态 |
| completion job | 需要主线程回调时，额外提交 `BIO_COMP_RQ_*` |
| 主线程通过 pipe 被唤醒 | 因为主线程阻塞在 `epoll`，不能用 `cond_wait` |

---

## 三、阶段 1：从初始化跟到 bioInit

### 3.1 启动时序

（与 [1.2.1](#121-启动期主线程创建-bio再进入事件循环) 相同逻辑；此处保留便于阶段 1 独立阅读。）

```mermaid
sequenceDiagram
    autonumber
    participant M as main_主线程
    participant IS as initServer
    participant IL as InitServerLast
    participant BI as bioInit
    participant LD as loadDataFromDisk
    participant AE as aeMain

    M->>IS: 创建ae事件循环_DB_记录main_thread_id
    Note over M,IS: 此时尚无BIO线程
    M->>IL: 模块加载_listener就绪后
    IL->>BI: pthread_create x3
    Note over BI: 三个 BIO worker 已常驻
    M->>LD: 加载RDB或AOF（7.4：BIO已存在）
    M->>AE: 进入事件循环
```

### 3.2 main() 中的关键片段（[server.c](../src/server.c)）

```c
int main(int argc, char **argv) {
    // ... 配置解析 initServerConfig() ...

    initServer();          // 【导读】创建 server.el（ae 事件循环）、DB、客户端列表
                           // 【导读】server.main_thread_id = pthread_self()  ← 标记主线程

    // ... 模块加载、initListeners() ...

    InitServerLast();      // 【导读】★ BIO 线程在这里创建（故意放在最后，避免 TLS/dlopen 竞态）

    loadDataFromDisk();    // 【注意】7.4 分支中 load 在 InitServerLast 之后，BIO 已存在

    // ... 打开监听 ...

    aeSetBeforeSleepProc(beforeSleep);
    aeSetAfterSleepProc(afterSleep);
    aeMain(server.el);     // 【导读】主线程进入 epoll 循环；pipe 可读时调 bioPipeReadJobCompList
}
```

> **阅读提示**：以你本地 `server.c` 中 `InitServerLast()` 与 `loadDataFromDisk()` 的先后顺序为准（不同版本可能略有调整）。核心是：**BIO 在对外服务前创建，且依赖 `server.el` 已存在**。

### 3.3 initServer() 中与 BIO 相关的准备（[server.c:2592](../src/server.c)）

```c
void initServer(void) {
    ThreadsManager_init();           // 【导读】IO 线程信号协调，与 BIO 独立
    server.main_thread_id = pthread_self();  // 【导读】后续可用 pthread_equal 判断是否主线程

    server.el = aeCreateEventLoop(...);      // 【导读】★ 创建主线程事件循环「空架子」；bioInit 要把 pipe 注册到其上
    // ... 创建 DB、客户端列表等 ...
    aeCreateTimeEvent(server.el, 1, serverCron, ...);  // 【导读】时间事件：约每 1ms 跑 serverCron
    aeSetBeforeSleepProc(server.el, beforeSleep);      // 【导读】每轮 epoll 前：含 flushAppendOnlyFile
}
```

> **易混点**：名字是 **Event** Loop（事件循环），不是「时间循环」。其中既有 **文件事件**（fd 可读可写），也有 **时间事件**（定时回调）。详见 [十、常见疑问 Q2–Q3](#十常见疑问faq)。

### 3.4 InitServerLast()（[server.c:2881](../src/server.c)）

```c
void InitServerLast(void) {
    bioInit();              // 【导读】★ 创建 3 个 BIO pthread
    initThreadedIO();       // 【导读】IO 线程（网络读写），与 BIO 是另一套线程
    set_jemalloc_bg_thread(...);
    server.initial_memory_usage = zmalloc_used_memory();
}
```

**为何单独一个函数？** 注释说明：等模块 `dlopen` 完成后再创建线程，避免 TLS 初始化竞态（[server.c:2876–2880](../src/server.c)）。

---

## 四、阶段 2：bioInit 与 worker 内核（带注释导读）

### 4.1 bioInit() 逐步导读（[bio.c:124](../src/bio.c)）

```c
void bioInit(void) {
    // ━━━ 步骤1：每个 worker 一套「mutex + cond + 队列」━━━
    for (j = 0; j < BIO_WORKER_NUM; j++) {
        pthread_mutex_init(&bio_mutex[j], NULL);
        pthread_cond_init(&bio_newjob_cond[j], NULL);
        bio_jobs[j] = listCreate();
    }
    // 【嵌入式对照】≈ 为每个后台 Task 建一个 msgQ + 互斥锁

    // ━━━ 步骤2：completion 响应队列（主线程回调用）━━━
    bio_comp_list = listCreate();
    pthread_mutex_init(&bio_mutex_comp, NULL);

    // ━━━ 步骤3：pipe — 桥接 BIO 线程与主线程 ae 事件循环 ━━━
    anetPipe(job_comp_pipe, O_CLOEXEC|O_NONBLOCK, O_CLOEXEC|O_NONBLOCK);
    aeCreateFileEvent(server.el, job_comp_pipe[0], AE_READABLE,
                      bioPipeReadJobCompList, NULL);
    // 【导读】主线程 epoll 阻塞时，靠 pipe 可读事件唤醒，不能用 pthread_cond

    // ━━━ 步骤4：栈扩大到 4MB（lazyfree 深调用栈）━━━
    pthread_attr_setstacksize(&attr, stacksize);  // REDIS_THREAD_STACK_SIZE

    // ━━━ 步骤5：创建 3 个 worker 线程 ━━━
    for (j = 0; j < BIO_WORKER_NUM; j++) {
        pthread_create(&thread, &attr, bioProcessBackgroundJobs, (void*)j);
        bio_threads[j] = thread;
    }
}
```

### 4.2 job 路由表（[bio.c:58–66](../src/bio.c)）

```c
static unsigned int bio_job_to_worker[] = {
    [BIO_CLOSE_FILE]         = 0,   // → bio_close_file
    [BIO_AOF_FSYNC]          = 1,   // → bio_aof
    [BIO_CLOSE_AOF]          = 1,   // → bio_aof（与 fsync 同 worker，保证顺序）
    [BIO_LAZY_FREE]          = 2,   // → bio_lazy_free
    [BIO_COMP_RQ_CLOSE_FILE] = 0,
    [BIO_COMP_RQ_AOF_FSYNC]  = 1,
    [BIO_COMP_RQ_LAZY_FREE]  = 2,
};
```

**导读**：AOF 的 fsync 和 close 进**同一个 worker1**，避免同一 fd 上 fsync/close 乱序。

### 4.3 bioSubmitJob() — 生产者（[bio.c:178](../src/bio.c)）

```c
void bioSubmitJob(int type, bio_job *job) {
    job->header.type = type;
    unsigned long worker = bio_job_to_worker[type];

    pthread_mutex_lock(&bio_mutex[worker]);
    listAddNodeTail(bio_jobs[worker], job);   // 入队（FIFO）
    bio_jobs_counter[type]++;
    pthread_cond_signal(&bio_newjob_cond[worker]);  // 唤醒对应 worker
    pthread_mutex_unlock(&bio_mutex[worker]);
}
// 【嵌入式对照】≈ semTake(mtx); msgQSend(queue, job); semGive(mtx); 再唤醒 worker Task
```

**调用者**：仅 `bioCreate*` 系列函数（主线程路径），worker 自己不调用。

> **常见疑问**：为何入队不检查容量、一直 `listAdd`？——BIO 队列有意无界；反压在调用方（如 AOF 的 `aofFsyncInProgress`）。详见 [十、常见疑问 Q4](#十常见疑问faq)。

### 4.4 bioCreateFsyncJob() — 持久化最常用入口（[bio.c:244](../src/bio.c)）

```c
void bioCreateFsyncJob(int fd, long long offset, int need_reclaim_cache) {
    bio_job *job = zmalloc(sizeof(*job));
    job->fd_args.fd = fd;
    job->fd_args.offset = offset;       // fsync 成功后写入 fsynced_reploff_pending
    job->fd_args.need_reclaim_cache = need_reclaim_cache;
    bioSubmitJob(BIO_AOF_FSYNC, job);
}
```

**上游调用链**：

```
flushAppendOnlyFile()          [aof.c:1045]
  └─ aof_background_fsync()   [aof.c:905]
       └─ bioCreateFsyncJob()  [bio.c:244]
            └─ bioSubmitJob()
                 └─ worker1: bioProcessBackgroundJobs()
```

### 4.5 bioProcessBackgroundJobs() — 消费者（[bio.c:253](../src/bio.c)）

```c
void *bioProcessBackgroundJobs(void *arg) {
    unsigned long worker = (unsigned long) arg;  // 0/1/2

    redis_set_thread_title(bio_worker_title[worker]);  // 线程名：bio_aof 等
    pthread_sigmask(SIG_BLOCK, SIGALRM, ...);        // 只有主线程处理 watchdog

    pthread_mutex_lock(&bio_mutex[worker]);

    while (1) {
        // ─── 等待任务 ───
        if (listLength(bio_jobs[worker]) == 0) {
            pthread_cond_wait(&bio_newjob_cond[worker], &bio_mutex[worker]);
            continue;
        }

        ln = listFirst(bio_jobs[worker]);
        job = ln->value;
        pthread_mutex_unlock(&bio_mutex[worker]);  // 【导读】执行 job 时不持锁！

        // ─── 按类型执行 ───
        if (job_type == BIO_AOF_FSYNC || job_type == BIO_CLOSE_AOF) {
            redis_fsync(job->fd_args.fd);          // 【慢操作】在后台做
            atomicSet(server.fsynced_reploff_pending, job->fd_args.offset);
            atomicSet(server.aof_bio_fsync_status, C_OK);
            if (job_type == BIO_CLOSE_AOF) close(fd);
        } else if (job_type == BIO_CLOSE_FILE) {
            // 可选先 fsync，再 close
            close(job->fd_args.fd);
        } else if (job_type == BIO_LAZY_FREE) {
            job->free_args.free_fn(job->free_args.free_args);
        } else if (job_type == BIO_COMP_RQ_*) {
            // 写入 bio_comp_list，write(pipe) 唤醒主线程
            listAddNodeTail(bio_comp_list, comp_rsp);
            write(job_comp_pipe[1], "A", 1);
        }

        zfree(job);

        // ─── 从队列删除节点，重新加锁 ───
        pthread_mutex_lock(&bio_mutex[worker]);
        listDelNode(bio_jobs[worker], ln);
        bio_jobs_counter[job_type]--;
        pthread_cond_signal(&bio_newjob_cond[worker]);  // 唤醒 bioDrainWorker 等等待者
    }
}
```

**导读要点**：

| 要点 | 说明 |
|------|------|
| 持锁范围 | 只在「等队列 / 删节点」时持锁，**执行 job 时释放锁** |
| `bioDrainWorker` | 等的就是 `bio_jobs_counter` 变 0 + `cond_signal` |
| atomic 更新 | fsync 结果用 atomic 传给主线程，避免数据竞争 |

---

## 五、阶段 3：完成通知（pipe → ae）

### 5.1 为何需要 pipe？

```
主线程阻塞在：epoll_wait()  ← ae 事件循环
BIO 线程完成：需要通知主线程执行 comp_fn 回调

pthread_cond_signal(主线程)  ✗  主线程不在 cond_wait 上
write(pipe) → epoll 可读       ✓  集成进现有事件循环
```

`bioInit` 里成套创建的是：

| 对象 | 角色 |
|------|------|
| `BIO_COMP_RQ_*` job | 挂在 **worker 的 bio_jobs** 上，FIFO 栅栏：「排到我 = 前面的活做完了」 |
| `bio_comp_list` | 跨线程的 **回调载荷**（`func` + `arg`） |
| `job_comp_pipe` | 只负责 **叫醒** 主线程 ae，本身不带业务数据 |

> 普通 AOF fsync **不走** comp list（用 atomic 汇报）；显式 `bioCreateCompRq`（如 FLUSHALL ASYNC）才进 list。详见 [十、常见疑问 Q5](#十常见疑问faq)。

### 5.2 bioPipeReadJobCompList()（[bio.c:412](../src/bio.c)）

```c
void bioPipeReadJobCompList(aeEventLoop *el, int fd, ...) {
    // 读空 pipe（非阻塞）
    while (read(fd, buf, sizeof(buf)) == sizeof(buf));

    // 原子「换走」completion 列表，减少持锁时间
    pthread_mutex_lock(&bio_mutex_comp);
    if (listLength(bio_comp_list)) {
        tmp_list = bio_comp_list;
        bio_comp_list = listCreate();   // 新列表给 worker 继续写
    }
    pthread_mutex_unlock(&bio_mutex_comp);

    // 在主线程执行所有回调
    while (listLength(tmp_list)) {
        rsp->func(rsp->arg);   // 【导读】例如 FLUSHALL 异步完成通知客户端
        zfree(rsp);
    }
}
```

```mermaid
sequenceDiagram
    participant Main as 主线程_aeMain
    participant BIO as BIO_worker
    participant Pipe as job_comp_pipe

    BIO->>BIO: 执行BIO_COMP_RQ job
    BIO->>BIO: 写入bio_comp_list
    BIO->>Pipe: write("A")
    Pipe->>Main: epoll可读
    Main->>Main: bioPipeReadJobCompList()
    Main->>Main: 执行comp_fn回调
```

---

## 六、阶段 4：持久化相关业务调用点

### 6.1 AOF everysec fsync 全链路

（总览见 [1.2.2](#122-运行期主线程驱动-aof再把-fsync-交给-bio)；本节强调业务副作用。）

```mermaid
sequenceDiagram
    autonumber
    participant C as 客户端
    participant M as 主线程
    participant Sleep as beforeSleep
    participant AOF as aof.c
    participant BIO as bio_aof_worker

    C->>M: SET key value
    M->>M: 修改内存 + 追加aof_buf
    M->>Sleep: beforeSleep()
    Sleep->>AOF: flushAppendOnlyFile()
    AOF->>AOF: write(aof_fd) 主线程
    AOF->>AOF: aof_background_fsync()
    AOF->>BIO: bioCreateFsyncJob → bioSubmitJob
    Note over M: 主线程不等 fsync，继续 ae 循环
    BIO->>BIO: redis_fsync()
    BIO->>BIO: atomicSet(fsynced_reploff_pending)
```

| 步骤 | 文件:函数 | 说明 |
|------|-----------|------|
| 1 | aof.c:`feedAppendOnlyFile` | 命令追加到 `server.aof_buf` |
| 2 | aof.c:`flushAppendOnlyFile` | `write()` 到 `server.aof_fd` |
| 3 | aof.c:`aof_background_fsync` | 仅 `appendfsync everysec` 走 BIO |
| 4 | bio.c:`bioCreateFsyncJob` | 提交到 worker1 |
| 5 | bio.c worker | `redis_fsync()` + 更新 atomic 状态 |

### 6.2 AOF Rewrite 前的 bioDrainWorker

```c
// aof.c: rewriteAppendOnlyFileBackground()
bioDrainWorker(BIO_AOF_FSYNC);   // 【导读】等 worker1 队列里所有 fsync job 执行完
redisFork(CHILD_TYPE_AOF);        // 然后才 fork，避免 repl offset 竞态
```

**自检**：为何必须排空？见 [aof.c:2457–2463](../src/aof.c) 注释（`fsynced_reploff_pending` 与新旧 AOF 切换）。

### 6.3 bg_unlink → BIO close

```c
// replication.c: bg_unlink()
fd = open(filename, O_RDONLY);
unlink(filename);                  // 【导读】主线程：只删目录项（文件名）
bioCreateCloseJob(fd, 0, 0);       // 【导读】BIO worker0：close(fd) 真正释放 inode
```

---

## 七、阶段 5：动手验证

推荐优先用 **命令行** 完成验证（见 roadmap 1-A/1-B）。需要看线程/调用栈时再用 **lldb**（macOS）或可选 CLion；步骤与原因以 [rdb-aof-learning-roadmap.md 实验 1-C](rdb-aof-learning-roadmap.md) 为准。

### 7.1 编译与命令行启动（备选）

```bash
make CFLAGS="-g -O0"
./src/redis-server --appendonly yes --appendfsync everysec
```

### 7.2 （可选）CLion / lldb 交互调试

> 完整逐步操作与**每步原因**见 roadmap **实验 1-C**。此处只列要点，避免与旧「断函数入口」流程冲突。

#### 7.2.1–7.2.3 工程与 Run 配置

同前：Makefile 工程、`OPTIMIZATION=-O0 MALLOC=libc`、arguments 含 `--appendonly yes --appendfsync everysec`。macOS 调试器用 **LLDB**。

#### 7.2.4 断点怎么下（重要）

| 该下 | 不该下 | 原因 |
|------|--------|------|
| `bioCreateFsyncJob` / `bioSubmitJob` | — | 证明主线程投递；看 `type=1`、`worker=1` |
| `bio.c` 中 `BIO_AOF_FSYNC` 分支 **行断点**（约 `redis_fsync(job->fd_args.fd)` 那一行） | `bioProcessBackgroundJobs` **函数入口** | worker 早已在函数内 `while(1)`，入口不会再次命中 |
| — | `b redis_fsync`（macOS） | 是宏不是函数 |

流程摘要：先看生产者 → **disable** Create/Submit → 只留行断点 → `c` + 再压测 → `thread list` 确认非主线程。

#### 7.2.5 跟 BIO 时建议观察的界面

```mermaid
flowchart LR
  subgraph clion [Debug窗口]
    bp[断点列表]
    frames[Frames调用栈]
    threads[Threads线程列表]
  end
  subgraph flow [跟读顺序]
    create[bioCreateFsyncJob]
    submit[bioSubmitJob]
    line[bio.c_AOF_FSYNC行]
  end
  bp --> create --> submit --> line
  threads -->|"非主线程"| line
  frames -->|"主线程 aeMain"| submit
```

#### 7.2.6 lldb 与 gdb 对照

| 意图 | lldb（macOS） | gdb |
|------|---------------|-----|
| 启动 | `lldb -- ./src/redis-server ...` | `gdb --args ./src/redis-server ...` |
| 符号断点 | `b bioSubmitJob` | `break bioSubmitJob` |
| 行断点 | `b bio.c:323` | `break bio.c:323` |
| 继续 | `c` | `c` |
| 线程列表 | `thread list` | `info threads` |
| 调用栈 | `bt` | `bt` |
| 关断点 | `breakpoint disable N` | `disable N` |

### 7.3 lldb 最短命令序列（与 roadmap 1-C 一致）

```bash
lldb -- ./src/redis-server --port 6379 --dir /tmp/redis-lab1 \
  --appendonly yes --appendfsync everysec
```

```text
(lldb) b bioCreateFsyncJob
(lldb) b bioSubmitJob
(lldb) b bio.c:323
(lldb) run
# 另终端 benchmark → 停在 Submit 时: p type / 单步后 p worker
(lldb) breakpoint disable 1
(lldb) breakpoint disable 2
(lldb) c
# 再 benchmark → 停在 bio.c:323 时: thread list / bt
```

行号 `323` 请用编辑器搜索 `BIO_AOF_FSYNC` 核对。

### 7.4 每遍阅读的自检问题

| 遍次 | 问题 | 答案要点 |
|------|------|----------|
| 第 1 遍 | BIO 何时创建？ | `InitServerLast()` → `bioInit()`，3 个 pthread |
| 第 2 遍 | job 怎么提交与取出？ | `bioSubmitJob`：lock→入队→signal；worker：`cond_wait`→取队首 |
| 第 3 遍 | fsync 在哪个 worker？ | worker1 `bio_aof` |
| 第 4 遍 | 主线程如何被唤醒？ | `job_comp_pipe` → `ae` 可读 → `bioPipeReadJobCompList` |
| 第 5 遍 | Rewrite 前为何 drain？ | 等旧 AOF 的 fsync 全部完成，防 repl offset 竞态 |
| 第 6 遍 | AOF 与主线程如何关联？ | `aeMain → beforeSleep → flushAppendOnlyFile`（见 FAQ Q1） |
| 第 7 遍 | Submit 为何不限队列深度？ | 无界 list；反压在调用方（见 FAQ Q4） |
| 第 8 遍 | 如何确认 fsync 在 BIO？ | 对 `bio.c` AOF_FSYNC **行断点**命中后 `thread list` 非主线程（见 roadmap 1-C） |

---

## 八、函数关系速查表

| 函数 | 角色 | 直接调用者 | 直接调用 |
|------|------|------------|----------|
| `bioInit` | 初始化 | `InitServerLast` | `pthread_create`×3, `aeCreateFileEvent` |
| `bioSubmitJob` | 入队 | 所有 `bioCreate*` | `pthread_mutex/cond` |
| `bioCreateFsyncJob` | 创建 fsync job | `aof_background_fsync` | `bioSubmitJob` |
| `bioCreateCloseJob` | 创建 close job | `bg_unlink` 等 | `bioSubmitJob` |
| `bioCreateLazyFreeJob` | 创建 free job | `lazyfree.c` | `bioSubmitJob` |
| `bioProcessBackgroundJobs` | worker 循环 | `pthread` 入口 | `redis_fsync`, `close`, `write(pipe)` |
| `bioPipeReadJobCompList` | 主线程回调 | `ae` 事件循环 | `comp_fn` |
| `bioDrainWorker` | 同步等待 | `rewriteAppendOnlyFileBackground` | `cond_wait` |
| `bioPendingJobsOfType` | 查询队列深度 | `aofFsyncInProgress` 等 | — |
| `bioKillThreads` | 强制结束 | `debug.c`（crash） | `pthread_cancel` |

---

## 九、源码 `【导读】` 注释索引

> 在 IDE 中全局搜索 `【导读】` 可快速定位所有学习注释。

| 文件 | 关键位置 | 导读要点 |
|------|----------|----------|
| `src/bio.h` | `bio_worker_t` 枚举 | 三个 worker 分工 |
| `src/bio.h` | `bioInit` / `bioCreate*` API | 对外接口一览 |
| `src/bio.c` | `bio_job_to_worker[]` | job 路由规则 |
| `src/bio.c` | `bioInit()` | 创建 3 个 pthread、注册 pipe |
| `src/bio.c` | `bioSubmitJob()` | 生产者入队 |
| `src/bio.c` | `bioProcessBackgroundJobs()` | worker 消费者循环 |
| `src/bio.c` | `bioPipeReadJobCompList()` | pipe 唤醒主线程 ae |
| `src/bio.c` | `bioDrainWorker()` | rewrite 前排空 fsync |
| `src/server.c` | `initServer()` | 主线程 ID、创建 `server.el` |
| `src/server.c` | `InitServerLast()` | `bioInit` + `initThreadedIO` |
| `src/server.c` | `main()` | 启动顺序、`loadDataFromDisk` 时机 |
| `src/server.c` | `aeMain()` | 主循环处理 job_comp_pipe |
| `src/aof.c` | `aof_background_fsync()` | everysec 后台 fsync |
| `src/aof.c` | `flushAppendOnlyFile()` everysec 分支 | 提交 fsync job |
| `src/aof.c` | `rewriteAppendOnlyFileBackground()` | fork rewrite 与 drain |
| `src/replication.c` | `bg_unlink()` | unlink + BIO close |
| `src/lazyfree.c` | `freeObjAsync()` | 大 key 异步释放 |

---

## 十、常见疑问（FAQ）

> 本节汇总导读中的典型疑问与结论，与正文时序图互补。

### Q1：全局关系图里，主线程和 AOF 入口为何曾「看起来没关联」？

**疑问**：主线程应负责启动与调度，AOF 不该像一条独立启动链。

**结论**：原图把**启动期**与**运行期**拆开画时，漏了运行期边。实际上：

1. **启动**：主线程 `main → InitServerLast → bioInit`，一次性 `pthread_create` 出 3 个 BIO worker。
2. **运行**：主线程在 `aeMain` 里，每轮 `beforeSleep()` → `flushAppendOnlyFile()`；`everysec` 再 `aof_background_fsync → bioCreateFsyncJob`。

AOF 不是另起线程，而是主线程事件循环里的业务调用；BIO 只是已创建的 worker。正确画法见 [1.2](#12-全局关系图时序优先)。

---

### Q2：`aeMain` 是什么流程？

**结论**：Redis **主线程事件循环**。启动结束后主线程几乎一直待在这里：

```c
void aeMain(aeEventLoop *eventLoop) {
    while (!eventLoop->stop) {
        aeProcessEvents(...);  // beforeSleep → epoll_wait → 文件/时间事件回调
    }
}
```

每轮要点：

1. `beforeSleep`（含 AOF flush、写回客户端等）
2. `aeApiPoll`（epoll/kqueue 阻塞）
3. 文件事件回调（客户端、`job_comp_pipe` 等）
4. 时间事件（`serverCron`）

**嵌入式对照**：不是再 `taskSpawn` 一个主循环任务，而是主线程自己 `while(1)` 在 mux 上等待——类似「一个任务挂多个事件源」。**调度中枢是 `aeMain`，BIO 只是后台 worker。**

---

### Q3：`initServer` 里的 `aeCreateEventLoop` 有何作用？「时间」如何理解？后续怎么用？

**结论**：创建的是 **Event Loop（事件循环）对象** `server.el`，此时**还不跑循环**，只造好「空架子」。

| 概念 | 含义 | Redis 例子 |
|------|------|------------|
| **文件事件** | fd 可读/可写时调回调 | 客户端连接、`job_comp_pipe`、监听口 |
| **时间事件** | 过 N 毫秒调一次 | `aeCreateTimeEvent(..., 1, serverCron)` |
| **epoll 超时** | poll 最长睡多久 | 睡到下一时间事件，或有 fd 就绪 |

后续挂载与运转：

```mermaid
sequenceDiagram
    autonumber
    participant Init as initServer
    participant EL as server.el
    participant Bio as bioInit
    participant Main as aeMain

    Init->>EL: aeCreateEventLoop（空架子）
    Init->>EL: TimeEvent→serverCron；BeforeSleep→beforeSleep
    Note over Init,EL: 尚未进入循环
    Bio->>EL: FileEvent→job_comp_pipe
    Main->>EL: aeMain 真正 while 调度
```

所以必须先有 `aeCreateEventLoop`，`bioInit` 才能把 pipe 注册上去；最后 `aeMain(server.el)` 才开始调度。

---

### Q4：`bioSubmitJob` 作为生产者，为何不检查 job 数量是否超出规格，而是一直添加？

**结论**：有意做成**无界 FIFO**；限流不在 Submit 层。

原因简述：

1. **职责薄**：只保证入队 + 唤醒 + 同 worker FIFO。满时该丢/阻塞/报错？三类 job 语义不同，通用 API 难统一。
2. **正常负载稀**：healthy 时 everysec 大约每秒 1 个 fsync；不像 RTOS 高频 `msgQSend` 要硬顶 `maxMsgs`。
3. **主线程不能被 BIO 堵死**：若队列满就在 Submit 里 `cond_wait`，慢盘会反压回事件循环，违背 BIO 初衷。

反压在**调用方**：

| 路径 | 做法 |
|------|------|
| AOF everysec | 已有 pending fsync 时不再提交（`!sync_in_progress`） |
| AOF write | fsync 未完成可推迟 write 最多约 2s |
| 淘汰 | 可短等 `bioPendingJobsOfType(BIO_LAZY_FREE)` |
| 运维 | `INFO` 的 `aof_pending_bio_fsync` |

极端堆积靠监控/内存暴露，而不是 Submit 返回 full。

| | VxWorks `msgQ` | Redis BIO |
|--|----------------|-----------|
| 队列 | 常有 `maxMsgs` | 无界 `list` |
| 满时策略 | 阻塞 / 超时 / 丢 | 不在 Submit 定义 |
| 安全阀 | 队列深度 | 调用方节流 + 内存/变慢 |

---

### Q5：`bioInit` 里创建的 `bio_comp_list`（comp list）作用是什么？

**结论**：BIO 做完后，若回调**必须回主线程**执行，就用这条队列投递；与 pipe 成套。

主线程堵在 `epoll_wait`，不能用 `pthread_cond` 唤醒。普通 fsync/close/lazyfree 做完多半只改 atomic 或释放内存；需要「等 worker 上前面 job 清完再在主线程做事」时：

1. `bioCreateCompRq` → 往该 worker 塞 `BIO_COMP_RQ_*`（FIFO 栅栏）
2. worker 赶到它 → 前面已做完
3. 回调写入 `bio_comp_list`，`write(job_comp_pipe)` 叫醒 ae
4. 主线程 `bioPipeReadJobCompList` 取表执行 `comp_fn`

```mermaid
sequenceDiagram
    autonumber
    participant Main as 主线程
    participant W as BIO_worker
    participant Comp as bio_comp_list
    participant Pipe as job_comp_pipe

    Main->>W: 真实 job（如 lazyfree）
    Main->>W: bioCreateCompRq（排在后面）
    W->>W: FIFO 做完真实 job，再处理 COMP_RQ
    W->>Comp: listAdd(func, arg)
    W->>Pipe: write("A")
    Pipe->>Main: epoll 可读
    Main->>Comp: 取走整表，执行 func(arg)
```

典型：`FLUSHALL` blocking async（[db.c](../src/db.c)）在 lazyfree worker 上挂 `flushallSyncBgDone`。  
**everysec 的 `bioCreateFsyncJob` 一般不走 comp list**，靠 `fsynced_reploff_pending` 等 atomic。

**一句话**：`bio_comp_list` = 回调载荷；`pipe` = 叫醒信号；`BIO_COMP_RQ_*` = worker 队列上的完成栅栏。

---

## 相关文档

- [rdb-aof-learning-roadmap.md](rdb-aof-learning-roadmap.md) — 阶段 1 概要
- [rdb-aof-beginner-guide.md](rdb-aof-beginner-guide.md) — BIO 与 fork 双机制
- [pthread-vs-embedded.md](pthread-vs-embedded.md) — mutex+cond 对照
