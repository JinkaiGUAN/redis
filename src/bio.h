/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 */

#ifndef __BIO_H
#define __BIO_H

typedef void lazy_free_fn(void *args[]);
typedef void comp_fn(uint64_t user_data);

/* 【导读】BIO 三个常驻 worker，job 按类型路由到对应队列（见 bio.c bio_job_to_worker）。 */
typedef enum bio_worker_t {
    BIO_WORKER_CLOSE_FILE = 0,  /* 【导读】worker0：后台 close（bg_unlink 等） */
    BIO_WORKER_AOF_FSYNC,         /* 【导读】worker1：AOF fsync / close-AOF */
    BIO_WORKER_LAZY_FREE,       /* 【导读】worker2：lazyfree 大对象 */
    BIO_WORKER_NUM
} bio_worker_t;

/* Background job opcodes */
typedef enum bio_job_type_t {
    BIO_CLOSE_FILE = 0,     /* Deferred close(2) syscall. */
    BIO_AOF_FSYNC,          /* Deferred AOF fsync. */
    BIO_LAZY_FREE,          /* Deferred objects freeing. */
    BIO_CLOSE_AOF,
    BIO_COMP_RQ_CLOSE_FILE,  /* Job completion request, registered on close-file worker's queue */
    BIO_COMP_RQ_AOF_FSYNC,  /* Job completion request, registered on aof-fsync worker's queue */
    BIO_COMP_RQ_LAZY_FREE,  /* Job completion request, registered on lazy-free worker's queue */
    BIO_NUM_OPS
} bio_job_type_t;

/* Exported API — 【导读】不暴露 bio_job 内部结构，仅提供创建 job 与管理 worker 接口 */
void bioInit(void);                              /* 【导读】启动时创建 3 个 BIO 线程 */
unsigned long bioPendingJobsOfType(int type);    /* 【导读】查询某类 job 待处理数量 */
void bioDrainWorker(int job_type);               /* 【导读】阻塞直到对应 worker 队列排空 */
void bioKillThreads(void);
void bioCreateCloseJob(int fd, int need_fsync, int need_reclaim_cache);   /* 【导读】持久化：后台 close */
void bioCreateCloseAofJob(int fd, long long offset, int need_reclaim_cache);
void bioCreateFsyncJob(int fd, long long offset, int need_reclaim_cache); /* 【导读】持久化：AOF fsync */
void bioCreateLazyFreeJob(lazy_free_fn free_fn, int arg_count, ...);       /* 【导读】大 key 删除 */
void bioCreateCompRq(bio_worker_t assigned_worker, comp_fn *func, uint64_t user_data);


#endif
