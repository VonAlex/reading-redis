/* Background I/O service for Redis.
 * redis 后台 I/O 服务
 *
 * This file implements operations that we need to perform in the background.
 *  这个文件实现了我们需要在后台做的一些操作。
 *
 * Currently there is only a single operation, that is a background close(2)
 * system call.
 * 现在只有一个操作，就是后台 close(2) 系统调用（还有 fsync 刷盘，以及惰性删除）
 *
 * This is needed as when the process is the last owner of a
 * reference to a file closing it means unlinking it, and the deletion of the
 * file is slow, blocking the server.
 * 当本进程是一个文件引用的最后一个 owner 时，close 代表着 unlinking，这时删除操作会变慢，会阻塞 server。
 * 因此这个后台服务是必要的。
 *
 * In the future we'll either continue implementing new things we need or
 * we'll switch to libeio. However there are probably long term uses for this
 * file as we may want to put here Redis specific background tasks (for instance
 * it is not impossible that we'll need a non blocking FLUSHDB/FLUSHALL
 * implementation).
 * 以后我们将继续为其增加新的事情，或者我们会切换到 libeio。不过我们会长期使用这个文件，因为我们想把一些特殊的后台任务放在这里。
 * 比如，我们可能需要一个非阻塞的 FLUSHDB/FLUSHALL 实现。（惰性删除，在 4.x 已经有了）
 *
 * DESIGN
 * ------
 * The design is trivial, we have a structure representing a job to perform
 * and a different thread and job queue for every job type.
 * Every thread wait for new jobs in its queue, and process every job
 * sequentially.
 * 设计很简单。
 * 有个结构体表示要处理的 job。
 * 每个类型的 job 都有一个不同的线程和 queue。
 * 每个线程等待它的 job queue 里有新的 job，且顺序执行每一个 job
 *
 * Jobs of the same type are guaranteed to be processed from the least
 * recently inserted to the most recently inserted (older jobs processed
 * first).
 * 同一类型的工作按 FIFO 的顺序执行。
 *
 * Currently there is no way for the creator of the job to be notified about
 * the completion of the operation, this will only be added when/if needed.
 * 目前还没有办法在任务完成时通知执行者，在有需要的时候，会实现这个功能。
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include "server.h"
#include "bio.h"

static pthread_t bio_threads[BIO_NUM_OPS];
static pthread_mutex_t bio_mutex[BIO_NUM_OPS];
static pthread_cond_t bio_condvar[BIO_NUM_OPS];
static list *bio_jobs[BIO_NUM_OPS];
/* The following array is used to hold the number of pending jobs for every
 * OP type. This allows us to export the bioPendingJobsOfType() API that is
 * useful when the main thread wants to perform some operation that may involve
 * objects shared with the background thread. The main thread will just wait
 * that there are no longer jobs of this type to be executed before performing
 * the sensible operation. This data is also useful for reporting.
 *
 * 下面的数组用来存放每种 OP pending jobs 的数量。
 * 借此导出了 bioPendingJobsOfType() 这个 API，当主线程想要做某个操作涉及到与后台线程有共享 objects 时，这个 API 非常有用。
 * 主线程只需要等待没有这种类型的 job 后，再执行操作。
 * 这个数据也有利于 reporting。
 * */
static unsigned long long bio_pending[BIO_NUM_OPS];

/* This structure represents a background Job. It is only used locally to this
 * file as the API does not expose the internals at all.
 *
 * 这个结构体表示一个后台任务。
 * 且仅仅对本文件有用，因为 API 不对外开放。
 * */
struct bio_job {
    time_t time; /* Time at which the job was created. */
    /* Job specific arguments pointers. If we need to pass more than three
     * arguments we can just pass a pointer to a structure or alike.
     * 任务的参数。参数多于三个时，我们可以传递结构体指针
     * */
    void *arg1, *arg2, *arg3;
};

void *bioProcessBackgroundJobs(void *arg);

/* Make sure we have enough stack to perform all the things we do in the
 * main thread.
 * 子线程栈大小
 * */
#define REDIS_THREAD_STACK_SIZE (1024*1024*4)

/* Initialize the background system, spawning the thread.
 * 初始化后台系统，创建线程
 * */
void bioInit(void) {
    pthread_attr_t attr;
    pthread_t thread;
    size_t stacksize;
    int j;

    /* Initialization of state vars and objects
    * 初始化各类型job的锁和条件变量
    * */
    for (j = 0; j < BIO_NUM_OPS; j++) {
        pthread_mutex_init(&bio_mutex[j],NULL); // 锁
        pthread_cond_init(&bio_condvar[j],NULL);// 条件变量
        bio_jobs[j] = listCreate();
        bio_pending[j] = 0;
    }

    /* Set the stack size as by default it may be small in some system
    * 设置 stack 大小，因为某些系统默认值可能很小
    * */
    pthread_attr_init(&attr);
    pthread_attr_getstacksize(&attr,&stacksize);
    if (!stacksize) stacksize = 1; /* The world is full of Solaris Fixes */
    while (stacksize < REDIS_THREAD_STACK_SIZE) stacksize *= 2;
    pthread_attr_setstacksize(&attr, stacksize);

    /* Ready to spawn our threads. We use the single argument the thread
     * function accepts in order to pass the job ID the thread is
     * responsible of.
     * 创建线程。
     * 我们使用了单一参数值来作为函数的参数，以便 pass 这个线程所表示的 job ID（数组接收）
     * */
    for (j = 0; j < BIO_NUM_OPS; j++) {
        void *arg = (void*)(unsigned long) j;
        if (pthread_create(&thread,&attr,bioProcessBackgroundJobs,arg) != 0) {
            serverLog(LL_WARNING,"Fatal: Can't initialize Background Jobs.");
            exit(1);
        }
        bio_threads[j] = thread;
    }
}

// 创建 job
void bioCreateBackgroundJob(int type, void *arg1, void *arg2, void *arg3) {

    struct bio_job *job = zmalloc(sizeof(*job));
    job->time = time(NULL);
    job->arg1 = arg1;
    job->arg2 = arg2;
    job->arg3 = arg3;

    pthread_mutex_lock(&bio_mutex[type]); // 加锁
    listAddNodeTail(bio_jobs[type],job); // 将新的 job 放到对应 job queue 中
    bio_pending[type]++;
    pthread_cond_signal(&bio_condvar[type]); // 条件变量通知有新的 job 产生
    pthread_mutex_unlock(&bio_mutex[type]);// 解锁
}

// 处理后台任务，执行队列中的任务
void *bioProcessBackgroundJobs(void *arg) {
    struct bio_job *job;
    unsigned long type = (unsigned long) arg;// 传入参数为 job type
    sigset_t sigset;

    /* Check that the type is within the right interval.
     * 检查传入 type 的合理性
     * */
    if (type >= BIO_NUM_OPS) {
        serverLog(LL_WARNING,
            "Warning: bio thread started with wrong type %lu",type);
        return NULL;
    }

    /* Make the thread killable at any time, so that bioKillThreads()
     * can work reliably.
     * 设置线程可以被其他线程调用pthread_cancel函数取消/终止
     * （其他线程发来 cancel 请求）
     */
    // 设置本线程对cancle信号的反应
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL); 
    // 收到信号后继续运行至下一个取消点再退出，默认是立即退出，与 PTHREAD_CANCEL_ENABLE 配合使用
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    pthread_mutex_lock(&bio_mutex[type]);
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
        if (listLength(bio_jobs[type]) == 0) {
            /* 当没有 type 类型的任务时，会阻塞在这里，等待条件变量触发 */
            // 条件判断， 进入函数先解锁，停止阻塞, 又会重新加锁
            pthread_cond_wait(&bio_condvar[type],&bio_mutex[type]);
            continue;
        }
        /* Pop the job from the queue. */
        ln = listFirst(bio_jobs[type]);
        job = ln->value;
        /* It is now possible to unlock the background system as we know have
         * a stand alone job structure to process.*/
        pthread_mutex_unlock(&bio_mutex[type]); // 取完任务后就可以解锁了

       /* Process the job accordingly to its type.
        * 不同类型的 job 做不同的处理，调用 close 或者 fsync
        * */
        if (type == BIO_CLOSE_FILE) {
            close((long)job->arg1);
        } else if (type == BIO_AOF_FSYNC) {
            aof_fsync((long)job->arg1);
        } else {
            serverPanic("Wrong job type in bioProcessBackgroundJobs().");
        }
        zfree(job);

        /* Lock again before reiterating the loop, if there are no longer
         * jobs to process we'll block again in pthread_cond_wait().
         * 再一次加锁，在重入这个 loop 之前。
         * 如果没有 job 需要执行，我们将再一次在 pthread_cond_wait() 阻塞住。
         * */
        pthread_mutex_lock(&bio_mutex[type]);
        listDelNode(bio_jobs[type],ln);
        bio_pending[type]--;
    }
}

/* Return the number of pending jobs of the specified type.
 * 返回特定类型还未处理的 job 数量，从 bio_pending 数组里拿统计值
 * */
unsigned long long bioPendingJobsOfType(int type) {
    unsigned long long val;
    pthread_mutex_lock(&bio_mutex[type]);
    val = bio_pending[type];
    pthread_mutex_unlock(&bio_mutex[type]);
    return val;
}

/* Kill the running bio threads in an unclean way. This function should be
 * used only when it's critical to stop the threads for some reason.
 * Currently Redis does this only on crash (for instance on SIGSEGV) in order
 * to perform a fast memory check without other threads messing with memory.
 *
 * 以一种不友好的方式杀死正在运行的 bio 线程。
 * 这个函数应当仅在由于某种原因需要紧急停止后台线程时调用。
 * 现在 redis 仅仅在崩溃的时候回使用。为了在其他线程不干扰内存的情况下做一个快速的内存检查。
 * */
void bioKillThreads(void) {
    int err, j;

    for (j = 0; j < BIO_NUM_OPS; j++) {
        if (pthread_cancel(bio_threads[j]) == 0) {
            if ((err = pthread_join(bio_threads[j],NULL)) != 0) {
                serverLog(LL_WARNING,
                    "Bio thread for job type #%d can be joined: %s",
                        j, strerror(err));
            } else {
                serverLog(LL_WARNING,
                    "Bio thread for job type #%d terminated",j);
            }
        }
    }
}
