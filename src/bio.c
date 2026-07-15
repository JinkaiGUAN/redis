/* Background I/O service for Redis.
 *
 * This file implements operations that we need to perform in the background.
 * Currently there are 3 operations:
 * 1) a background close(2) system call. This is needed when the process is
 *    the last owner of a reference to a file closing it means unlinking it, and
 *    the deletion of the file is slow, blocking the server.
 * 2) AOF fsync
 * 3) lazyfree of memory
 *
 * In the future we'll either continue implementing new things we need or
 * we'll switch to libeio. However there are probably long term uses for this
 * file as we may want to put here Redis specific background tasks.
 *
 * DESIGN
 * ------
 *
 * The design is simple: We have a structure representing a job to perform,
 * and several worker threads and job queues. Every job type is assigned to
 * a specific worker thread, and a single worker may handle several different
 * job types.
 * Every thread waits for new jobs in its queue, and processes every job
 * sequentially.
 *
 * Jobs handled by the same worker are guaranteed to be processed from the
 * least-recently-inserted to the most-recently-inserted (older jobs processed
 * first).
 *
 * To let the creator of the job to be notified about the completion of the 
 * operation, it will need to submit additional dummy job, coined as
 * completion job request that will be written back eventually, by the
 * background thread, into completion job response queue. This notification
 * layout can simplify flows that might submit more than one job, such as
 * in case of FLUSHALL which for a single command submits multiple jobs. It
 * is also correct because jobs are processed in FIFO fashion.
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 */

#include "server.h"
#include "bio.h"
#include <fcntl.h>

static char* bio_worker_title[] = {
    "bio_close_file",
    "bio_aof",
    "bio_lazy_free",
};

#define BIO_WORKER_NUM (sizeof(bio_worker_title) / sizeof(*bio_worker_title))

/* 【导读】每种 job 路由到固定 worker；AOF fsync 与 close 共用 worker1，保证同 fd 操作顺序。 */
static unsigned int bio_job_to_worker[] = {
    [BIO_CLOSE_FILE] = 0,
    [BIO_AOF_FSYNC] = 1,
    [BIO_CLOSE_AOF] = 1,
    [BIO_LAZY_FREE] = 2,
    [BIO_COMP_RQ_CLOSE_FILE] = 0,
    [BIO_COMP_RQ_AOF_FSYNC]  = 1,
    [BIO_COMP_RQ_LAZY_FREE]  = 2
};

static pthread_t bio_threads[BIO_WORKER_NUM];
static pthread_mutex_t bio_mutex[BIO_WORKER_NUM];
static pthread_cond_t bio_newjob_cond[BIO_WORKER_NUM];
static list *bio_jobs[BIO_WORKER_NUM];
static unsigned long bio_jobs_counter[BIO_NUM_OPS] = {0};

/* 【导读】completion 回调队列；通过 job_comp_pipe 唤醒主线程 ae 事件循环。 */
static list *bio_comp_list;
static pthread_mutex_t bio_mutex_comp;
static int job_comp_pipe[2];   /* Pipe used to awake the event loop */

typedef struct bio_comp_item {
    comp_fn *func;    /* callback after completion job will be processed  */
    uint64_t arg;     /* user data to be passed to the function */
} bio_comp_item;

/* This structure represents a background Job. It is only used locally to this
 * file as the API does not expose the internals at all. */
typedef union bio_job {
    struct {
        int type; /* Job-type tag. This needs to appear as the first element in all union members. */
    } header;

    /* Job specific arguments.*/
    struct {
        int type;
        int fd; /* Fd for file based background jobs */
        long long offset; /* A job-specific offset, if applicable */
        unsigned need_fsync:1; /* A flag to indicate that a fsync is required before
                                * the file is closed. */
        unsigned need_reclaim_cache:1; /* A flag to indicate that reclaim cache is required before
                                * the file is closed. */
    } fd_args;

    struct {
        int type;
        lazy_free_fn *free_fn; /* Function that will free the provided arguments */
        void *free_args[]; /* List of arguments to be passed to the free function */
    } free_args;
    struct {
        int type; /* header */
        comp_fn *fn; /* callback. Handover to main thread to cb as notify for job completion */
        uint64_t arg; /* callback arguments */
    } comp_rq;
} bio_job;

void *bioProcessBackgroundJobs(void *arg);
void bioPipeReadJobCompList(aeEventLoop *el, int fd, void *privdata, int mask);

/* Make sure we have enough stack to perform all the things we do in the
 * main thread. */
#define REDIS_THREAD_STACK_SIZE (1024*1024*4)

/* Initialize the background system, spawning the thread.
 *
 * 【导读】由 InitServerLast() 调用（见 server.c），进程生命周期内创建 3 个常驻 pthread：
 *   worker0 (bio_close_file)：bg_unlink 等场景的后台 close。
 *   worker1 (bio_aof)：AOF fsync / close-AOF（appendfsync everysec）。
 *   worker2 (bio_lazy_free)：大对象/大库 lazyfree。
 * 主线程通过 bioSubmitJob() 提交任务；worker 执行 bioProcessBackgroundJobs()。
 * 完成回调经 job_comp_pipe 唤醒 ae（bioPipeReadJobCompList），不能用 cond 唤醒主线程（主线程在 epoll_wait 中）。 */
void bioInit(void) {
    pthread_attr_t attr;
    pthread_t thread;
    size_t stacksize;
    unsigned long j;

    /* 【导读】每个 worker 一套 mutex + cond + FIFO 队列（生产者-消费者）。 */
    for (j = 0; j < BIO_WORKER_NUM; j++) {
        pthread_mutex_init(&bio_mutex[j],NULL);
        pthread_cond_init(&bio_newjob_cond[j],NULL);
        bio_jobs[j] = listCreate();
    }

    /* init jobs comp responses */
    bio_comp_list = listCreate();
    pthread_mutex_init(&bio_mutex_comp, NULL);

    /* Create a pipe for background thread to be able to wake up the redis main thread.
     * Make the pipe non blocking. This is just a best effort aware mechanism
     * and we do not want to block not in the read nor in the write half.
     * Enable close-on-exec flag on pipes in case of the fork-exec system calls in
     * sentinels or redis servers. */
    if (anetPipe(job_comp_pipe, O_CLOEXEC|O_NONBLOCK, O_CLOEXEC|O_NONBLOCK) == -1) {
        serverLog(LL_WARNING,
                  "Can't create the pipe for bio thread: %s", strerror(errno));
        exit(1);
    }

    /* 【导读】将 pipe 注册到 initServer() 创建的 server.el；BIO 写 pipe，aeMain 分发 bioPipeReadJobCompList。 */
    if (aeCreateFileEvent(server.el, job_comp_pipe[0], AE_READABLE,
                          bioPipeReadJobCompList, NULL) == AE_ERR) {
        serverPanic("Error registering the readable event for the bio pipe.");
    }

    /* 【导读】lazyfree 等路径调用栈较深，需扩大线程栈（默认 4MB）。 */
    pthread_attr_init(&attr);
    pthread_attr_getstacksize(&attr,&stacksize);
    if (!stacksize) stacksize = 1; /* The world is full of Solaris Fixes */
    while (stacksize < REDIS_THREAD_STACK_SIZE) stacksize *= 2;
    pthread_attr_setstacksize(&attr, stacksize);

    /* Ready to spawn our threads. We use the single argument the thread
     * function accepts in order to pass the job ID the thread is
     * responsible for. */
    for (j = 0; j < BIO_WORKER_NUM; j++) {
        void *arg = (void*)(unsigned long) j;
        if (pthread_create(&thread,&attr,bioProcessBackgroundJobs,arg) != 0) {
            serverLog(LL_WARNING, "Fatal: Can't initialize Background Jobs. Error message: %s", strerror(errno));
            exit(1);
        }
        bio_threads[j] = thread;
    }
}

/* 【导读】主线程生产者：入队并 cond_signal 唤醒 worker；仅由本文件 bioCreate* 调用。 */
void bioSubmitJob(int type, bio_job *job) {
    job->header.type = type;
    unsigned long worker = bio_job_to_worker[type];
    pthread_mutex_lock(&bio_mutex[worker]);
    listAddNodeTail(bio_jobs[worker],job);
    bio_jobs_counter[type]++;
    pthread_cond_signal(&bio_newjob_cond[worker]);
    pthread_mutex_unlock(&bio_mutex[worker]);
}

/* 【导读】大 key/DB 删除 offload 到 worker2；调用方见 lazyfree.c。 */
void bioCreateLazyFreeJob(lazy_free_fn free_fn, int arg_count, ...) {
    va_list valist;
    /* Allocate memory for the job structure and all required
     * arguments */
    bio_job *job = zmalloc(sizeof(*job) + sizeof(void *) * (arg_count));
    job->free_args.free_fn = free_fn;

    va_start(valist, arg_count);
    for (int i = 0; i < arg_count; i++) {
        job->free_args.free_args[i] = va_arg(valist, void *);
    }
    va_end(valist);
    bioSubmitJob(BIO_LAZY_FREE, job);
}

/* 【导读】提交 completion 请求：worker 完成后经 pipe 在主线程执行 comp_fn（如 FLUSHALL 异步通知）。 */
void bioCreateCompRq(bio_worker_t assigned_worker, comp_fn *func, uint64_t user_data) {
    int type;
    switch (assigned_worker) {
        case BIO_WORKER_CLOSE_FILE:
            type = BIO_COMP_RQ_CLOSE_FILE;
            break;
        case BIO_WORKER_AOF_FSYNC:
            type = BIO_COMP_RQ_AOF_FSYNC;
            break;
        case BIO_WORKER_LAZY_FREE:
            type = BIO_COMP_RQ_LAZY_FREE;
            break;
        default:
            serverPanic("Invalid worker type in bioCreateCompRq().");
    }

    bio_job *job = zmalloc(sizeof(*job));
    job->comp_rq.fn = func;
    job->comp_rq.arg = user_data;
    bioSubmitJob(type, job);
}

/* 【导读】后台 close 交给 worker0；典型调用：bg_unlink() 在 unlink 之后。 */
void bioCreateCloseJob(int fd, int need_fsync, int need_reclaim_cache) {
    bio_job *job = zmalloc(sizeof(*job));
    job->fd_args.fd = fd;
    job->fd_args.need_fsync = need_fsync;
    job->fd_args.need_reclaim_cache = need_reclaim_cache;

    bioSubmitJob(BIO_CLOSE_FILE, job);
}

/* 【导读】AOF 轮转时 fsync+close，与 BIO_AOF_FSYNC 同 worker1 保证顺序。 */
void bioCreateCloseAofJob(int fd, long long offset, int need_reclaim_cache) {
    bio_job *job = zmalloc(sizeof(*job));
    job->fd_args.fd = fd;
    job->fd_args.offset = offset;
    job->fd_args.need_fsync = 1;
    job->fd_args.need_reclaim_cache = need_reclaim_cache;

    bioSubmitJob(BIO_CLOSE_AOF, job);
}

/* 【导读】提交 AOF fsync 到 worker1；appendfsync everysec 时由 aof_background_fsync 调用；主线程已完成 write()。 */
void bioCreateFsyncJob(int fd, long long offset, int need_reclaim_cache) {
    bio_job *job = zmalloc(sizeof(*job));
    job->fd_args.fd = fd;
    job->fd_args.offset = offset;
    job->fd_args.need_reclaim_cache = need_reclaim_cache;

    bioSubmitJob(BIO_AOF_FSYNC, job);
}

/* 【导读】BIO worker 入口：消费者循环。仅在等待/操作队列时持锁；fsync/close/lazyfree 等慢操作在锁外执行。 */
void *bioProcessBackgroundJobs(void *arg) {
    bio_job *job;
    unsigned long worker = (unsigned long) arg;
    sigset_t sigset;

    /* Check that the worker is within the right interval. */
    serverAssert(worker < BIO_WORKER_NUM);

    redis_set_thread_title(bio_worker_title[worker]);

    redisSetCpuAffinity(server.bio_cpulist);

    makeThreadKillable();

    pthread_mutex_lock(&bio_mutex[worker]);
    /* Block SIGALRM so we are sure that only the main thread will
     * receive the watchdog signal. */
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGALRM);
    if (pthread_sigmask(SIG_BLOCK, &sigset, NULL))
        serverLog(LL_WARNING,
            "Warning: can't mask SIGALRM in bio.c thread: %s", strerror(errno));

    while(1) {
        listNode *ln;

        /* The loop always starts with the lock hold. */
        if (listLength(bio_jobs[worker]) == 0) {
            pthread_cond_wait(&bio_newjob_cond[worker], &bio_mutex[worker]);
            continue;
        }
        /* Get the job from the queue. */
        ln = listFirst(bio_jobs[worker]);
        job = ln->value;
        /* 【导读】执行慢 I/O 前释放锁，允许主线程继续提交 job。 */
        pthread_mutex_unlock(&bio_mutex[worker]);

        /* Process the job accordingly to its type. */
        int job_type = job->header.type;

        if (job_type == BIO_CLOSE_FILE) {
            if (job->fd_args.need_fsync &&
                redis_fsync(job->fd_args.fd) == -1 &&
                errno != EBADF && errno != EINVAL)
            {
                serverLog(LL_WARNING, "Fail to fsync the AOF file: %s",strerror(errno));
            }
            if (job->fd_args.need_reclaim_cache) {
                if (reclaimFilePageCache(job->fd_args.fd, 0, 0) == -1) {
                    serverLog(LL_NOTICE,"Unable to reclaim page cache: %s", strerror(errno));
                }
            }
            close(job->fd_args.fd);
        } else if (job_type == BIO_AOF_FSYNC || job_type == BIO_CLOSE_AOF) {
            /* The fd may be closed by main thread and reused for another
             * socket, pipe, or file. We just ignore these errno because
             * aof fsync did not really fail. */
            if (redis_fsync(job->fd_args.fd) == -1 &&
                errno != EBADF && errno != EINVAL)
            {
                int last_status;
                atomicGet(server.aof_bio_fsync_status,last_status);
                atomicSet(server.aof_bio_fsync_status,C_ERR);
                atomicSet(server.aof_bio_fsync_errno,errno);
                if (last_status == C_OK) {
                    serverLog(LL_WARNING,
                        "Fail to fsync the AOF file: %s",strerror(errno));
                }
            } else {
                atomicSet(server.aof_bio_fsync_status,C_OK);
                /* 【导读】主线程读取（WAITAOF、复制等）。 */
                atomicSet(server.fsynced_reploff_pending, job->fd_args.offset);
            }

            if (job->fd_args.need_reclaim_cache) {
                if (reclaimFilePageCache(job->fd_args.fd, 0, 0) == -1) {
                    serverLog(LL_NOTICE,"Unable to reclaim page cache: %s", strerror(errno));
                }
            }
            if (job_type == BIO_CLOSE_AOF)
                close(job->fd_args.fd);
        } else if (job_type == BIO_LAZY_FREE) {
            job->free_args.free_fn(job->free_args.free_args);
        } else if ((job_type == BIO_COMP_RQ_CLOSE_FILE) ||
                   (job_type == BIO_COMP_RQ_AOF_FSYNC) ||
                   (job_type == BIO_COMP_RQ_LAZY_FREE)) {
            bio_comp_item *comp_rsp = zmalloc(sizeof(bio_comp_item));
            comp_rsp->func = job->comp_rq.fn;
            comp_rsp->arg = job->comp_rq.arg;

            /* 【导读】写入 completion 列表，再 write(pipe) 唤醒主线程 ae。 */
            pthread_mutex_lock(&bio_mutex_comp);
            listAddNodeTail(bio_comp_list, comp_rsp);
            pthread_mutex_unlock(&bio_mutex_comp);

            if (write(job_comp_pipe[1],"A",1) != 1) {
                /* Pipe is non-blocking, write() may fail if it's full. */
            }
        } else {
            serverPanic("Wrong job type in bioProcessBackgroundJobs().");
        }
        zfree(job);

        /* Lock again before reiterating the loop, if there are no longer
         * jobs to process we'll block again in pthread_cond_wait(). */
        pthread_mutex_lock(&bio_mutex[worker]);
        listDelNode(bio_jobs[worker], ln);
        bio_jobs_counter[job_type]--;
        /* 【导读】唤醒 bioDrainWorker() 中等待队列清空的主线程。 */
        pthread_cond_signal(&bio_newjob_cond[worker]);
    }
}

/* 【导读】供 aofFsyncInProgress() 等判断 BIO fsync 是否仍在队列中。 */
unsigned long bioPendingJobsOfType(int type) {
    unsigned int worker = bio_job_to_worker[type];

    pthread_mutex_lock(&bio_mutex[worker]);
    unsigned long val = bio_jobs_counter[type];
    pthread_mutex_unlock(&bio_mutex[worker]);

    return val;
}

/* Wait until a worker queue has no pending jobs of any type routed to it.
 * 【导读】主线程在 AOF rewrite 等场景调用，等待 worker 队列排空（如 bioDrainWorker(BIO_AOF_FSYNC)）。 */
void bioDrainWorker(int job_type) {
    unsigned long worker = bio_job_to_worker[job_type];

    pthread_mutex_lock(&bio_mutex[worker]);
    while (listLength(bio_jobs[worker]) > 0) {
        pthread_cond_wait(&bio_newjob_cond[worker], &bio_mutex[worker]);
    }
    pthread_mutex_unlock(&bio_mutex[worker]);
}

/* Kill the running bio threads in an unclean way. This function should be
 * used only when it's critical to stop the threads for some reason.
 * Currently Redis does this only on crash (for instance on SIGSEGV) in order
 * to perform a fast memory check without other threads messing with memory. */
void bioKillThreads(void) {
    int err;
    unsigned long j;

    for (j = 0; j < BIO_WORKER_NUM; j++) {
        if (bio_threads[j] == pthread_self()) continue;
        if (bio_threads[j] && pthread_cancel(bio_threads[j]) == 0) {
            if ((err = pthread_join(bio_threads[j],NULL)) != 0) {
                serverLog(LL_WARNING,
                    "Bio worker thread #%lu can not be joined: %s",
                        j, strerror(err));
            } else {
                serverLog(LL_WARNING,
                    "Bio worker thread #%lu terminated",j);
            }
        }
    }
}

/* 【导读】ae 可读事件：BIO worker 写 pipe 后，主线程批量执行 comp_fn。
 * 不能用 pthread_cond 唤醒主线程，因其阻塞在 aeMain/epoll_wait 中。 */
void bioPipeReadJobCompList(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    char buf[128];
    list *tmp_list = NULL;

    while (read(fd, buf, sizeof(buf)) == sizeof(buf));

    /* Handle event loop events if pipe was written from event loop API */
    pthread_mutex_lock(&bio_mutex_comp);
    if (listLength(bio_comp_list)) {
        tmp_list = bio_comp_list;
        bio_comp_list = listCreate();
    }
    pthread_mutex_unlock(&bio_mutex_comp);

    if (!tmp_list) return;

    /* callback to all job completions  */
    while (listLength(tmp_list)) {
        listNode *ln = listFirst(tmp_list);
        bio_comp_item *rsp = ln->value;
        listDelNode(tmp_list, ln);
        rsp->func(rsp->arg);
        zfree(rsp);
    }
    listRelease(tmp_list);
}
