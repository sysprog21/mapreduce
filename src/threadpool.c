/*
 * Copyright (c) 2016, Mathias Brossard <mathias@brossard.org>.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file threadpool.c
 * @brief Threadpool implementation file
 */

#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <semaphore.h>

#include "threadpool.h"
#define THREADS_MAX 255

typedef enum {
    immediate_shutdown = 1,
    graceful_shutdown  = 2
} threadpool_shutdown_t;

/**
 *  @struct threadpool_task
 *  @brief the work struct
 *
 *  @var function Pointer to the function that will perform the task.
 *  @var argument Argument to be passed to the function.
 */

typedef struct {
    void (*function)(void *);
    void *argument;
} threadpool_task_t;

/**
 *  @struct threadpool
 *  @brief The threadpool struct
 *
 *  @var notify       Condition variable to notify worker threads.
 *  @var threads      Array containing worker threads ID.
 *  @var thread_count Number of threads
 *  @var queue        Array containing the task queue.
 *  @var queue_size   Size of the task queue.
 *  @var head         Index of the first element.
 *  @var tail         Index of the next element.
 *  @var count        Number of pending tasks
 *  @var shutdown     Flag indicating if the pool is shutting down
 *  @var started      Number of started threads
 */
struct threadpool_t {
  pthread_mutex_t lock;
  pthread_cond_t notify;
  pthread_t *threads;
  threadpool_task_t *queue;
  int thread_count;
  int queue_size;
  int head;
  int tail;
  int count;
  int shutdown;
  int started;
};

/**
 * @function void *threadpool_thread(void *threadpool)
 * @brief the worker thread
 * @param threadpool the pool which own the thread
 */
static void *threadpool_thread(void *threadpool);

int threadpool_free(threadpool_t *pool);

threadpool_t *threadpool_create(int thread_count, int queue_size, int flags)
{
    threadpool_t *pool;
    int i;
    (void) flags;

    if(thread_count <= 0) return NULL;

    if(thread_count > MAX_THREADS || queue_size <= 0 || queue_size > MAX_QUEUE) {
        return NULL;
    }

    if((pool = (threadpool_t *)malloc(sizeof(threadpool_t))) == NULL) {
        goto err;
    }

    /* Initialize */
    pool->thread_count = 0;
    pool->queue_size = queue_size;
    pool->head = pool->tail = pool->count = 0;
    pool->shutdown = pool->started = 0;

    /* Allocate thread and task queue */
    pool->threads = (pthread_t *)malloc(sizeof(pthread_t) * thread_count);
    pool->queue = (threadpool_task_t *)malloc
        (sizeof(threadpool_task_t) * queue_size);

    /* Initialize mutex and conditional variable first */
    if((pthread_mutex_init(&(pool->lock), NULL) != 0) ||
       (pthread_cond_init(&(pool->notify), NULL) != 0) ||
       (pool->threads == NULL) ||
       (pool->queue == NULL)) {
        goto err;
    }

    /* Start worker threads */
    for(i = 0; i < thread_count; i++) {
        if(pthread_create(&(pool->threads[i]), NULL,
                          threadpool_thread, (void*)pool) != 0) {
            threadpool_destroy(pool, 0);
            return NULL;
        }
        pool->thread_count++;
        pool->started++;
    }

    return pool;

 err:
    if(pool) {
        threadpool_free(pool);
    }
    return NULL;
}

int threadpool_add(threadpool_t *pool, void (*function)(void *),
                   void *argument, int flags)
{
    int err = 0;
    int next;
    (void) flags;

    if(pool == NULL || function == NULL) {
        return threadpool_invalid;
    }

    if(pthread_mutex_lock(&(pool->lock)) != 0) {
        return threadpool_lock_failure;
    }

    next = (pool->tail + 1) % pool->queue_size;

    do {
        /* Are we full ? */
        if(pool->count == pool->queue_size) {
            err = threadpool_queue_full;
            break;
        }

        /* Are we shutting down ? */
        if(pool->shutdown) {
            err = threadpool_shutdown;
            break;
        }

        /* Add task to queue */
        pool->queue[pool->tail].function = function;
        pool->queue[pool->tail].argument = argument;
        pool->tail = next;
        pool->count += 1;

        /* pthread_cond_broadcast */
        if(pthread_cond_signal(&(pool->notify)) != 0) {
            err = threadpool_lock_failure;
            break;
        }
    } while(0);

    if(pthread_mutex_unlock(&pool->lock) != 0) {
        err = threadpool_lock_failure;
    }

    return err;
}

int threadpool_destroy(threadpool_t *pool, int flags)
{
    int i, err = 0;

    if(pool == NULL) {
        return threadpool_invalid;
    }

    if(pthread_mutex_lock(&(pool->lock)) != 0) {
        return threadpool_lock_failure;
    }

    do {
        /* Already shutting down */
        if(pool->shutdown) {
            err = threadpool_shutdown;
            break;
        }

        pool->shutdown = (flags & threadpool_graceful) ?
            graceful_shutdown : immediate_shutdown;

        /* Wake up all worker threads */
        if((pthread_cond_broadcast(&(pool->notify)) != 0) ||
           (pthread_mutex_unlock(&(pool->lock)) != 0)) {
            err = threadpool_lock_failure;
            break;
        }

        /* Join all worker thread */
        for(i = 0; i < pool->thread_count; i++) {
            if(pthread_join(pool->threads[i], NULL) != 0) {
                err = threadpool_thread_failure;
            }
        }
    } while(0);

    /* Only if everything went well do we deallocate the pool */
    if(!err) {
        threadpool_free(pool);
    }
    return err;
}

int threadpool_free(threadpool_t *pool)
{
    if(pool == NULL || pool->started > 0) {
        return -1;
    }

    /* Did we manage to allocate ? */
    if(pool->threads) {
        free(pool->threads);
        free(pool->queue);
 
        /* Because we allocate pool->threads after initializing the
           mutex and condition variable, we're sure they're
           initialized. Let's lock the mutex just in case. */
        pthread_mutex_lock(&(pool->lock));
        pthread_mutex_destroy(&(pool->lock));
        pthread_cond_destroy(&(pool->notify));
    }
    free(pool);    
    return 0;
}

typedef struct {
    int personal_pointers[THREADS_MAX];
    void(*routine)(int n, void *);
    void *arg;
    int size;
    int thread_count;
    sem_t done_indicator;
} threadpool_map_t;

static void threadpool_map_thread(void *arg);

int threadpool_map(threadpool_t *pool, int size, void(*routine)(int n, void *),
	void *arg, int flags)
{
    threadpool_error_t err = 0;
    threadpool_map_t map = {
        .routine = routine,
        .arg = arg,
        .thread_count = pool->thread_count,
        .size = size,
    };

    sem_init(&map.done_indicator, 0, 0);
	
    for (int i = 0; i < pool->thread_count; i++)
        map.personal_pointers[i] = i;

    for (int i = 0; i < pool->thread_count; i++) {
        threadpool_error_t _err = threadpool_add(pool, threadpool_map_thread,
                                                 &map.personal_pointers[i],
                                                 flags);
        /* FIXME: check errors correctly */
        if (_err) {
            err = _err;
            sem_post(&map.done_indicator);
        }
    }

    for (int i = 0; i < pool->thread_count; i++)
        sem_wait(&map.done_indicator);

    sem_destroy(&map.done_indicator);
    return err;
}

typedef struct {
    threadpool_reduce_t *userdata;
    int size;
    int thread_count;
    void *elements[THREADS_MAX];
} reduce_t_internal;

static void threadpool_reduce_thread(int n, void *arg);

int threadpool_reduce(threadpool_t *pool, threadpool_reduce_t *reduce)
{
    reduce_t_internal info = {
        .size = ((char *) reduce->end -
                 (char *) reduce->begin) / reduce->object_size,
        .thread_count = pool->thread_count,
        .userdata = reduce,
    };

    int err = threadpool_map(pool, pool->thread_count, threadpool_reduce_thread, &info, 0);
    if (err) return err;

    for (int i = 1; i < pool->thread_count; i++) {
        info.userdata->reduce(info.userdata->self,
                              info.elements[0], info.elements[i]);
        info.userdata->reduce_free(info.userdata->self, info.elements[i]);
    }
    info.userdata->reduce_finish(info.userdata->self, info.elements[0]);
    info.userdata->reduce_free(info.userdata->self, info.elements[0]);
    return 0;
}

static void threadpool_reduce_thread(int n, void *arg)
{
    reduce_t_internal *info = (reduce_t_internal *) arg;
    int end = info->size / info->thread_count;
    int additional_items = info->size - end * info->thread_count;
    int start = end * n;

    if (n <= additional_items)	{
        start += n;
        if (n < additional_items) end++;
    }
    else {
        start += additional_items;
    }
    end += start;

    info->elements[n] = info->userdata->reduce_alloc_neutral(info->userdata);

    for (; start < end; start++) {
        info->userdata->reduce(info->userdata->self, info->elements[n], 
                               (char *) info->userdata->begin +
                               start * info->userdata->object_size);
	}
}

static void threadpool_map_thread(void *arg)
{
    int id = *(int *) arg;
    threadpool_map_t *map = (threadpool_map_t *) ((int *) arg - id);
    int end = map->size / map->thread_count;
    int additional_items = map->size - end * map->thread_count;
    int start = end * id;
	
    if (id <= additional_items)	{
        start += id;
        if (id < additional_items) end++;
    }
    else {
        start += additional_items;
    }
    end += start;

    for (; start < end; start++)
        map->routine(start, map->arg);

    sem_post(&map->done_indicator);	
}

static void *threadpool_thread(void *threadpool)
{
    threadpool_t *pool = (threadpool_t *)threadpool;
    threadpool_task_t task;

    for(;;) {
        /* Lock must be taken to wait on conditional variable */
        pthread_mutex_lock(&(pool->lock));

        /* Wait on condition variable, check for spurious wakeups.
           When returning from pthread_cond_wait(), we own the lock. */
        while((pool->count == 0) && (!pool->shutdown)) {
            pthread_cond_wait(&(pool->notify), &(pool->lock));
        }

        if((pool->shutdown == immediate_shutdown) ||
           ((pool->shutdown == graceful_shutdown) &&
            (pool->count == 0))) {
            break;
        }

        /* Grab our task */
        task.function = pool->queue[pool->head].function;
        task.argument = pool->queue[pool->head].argument;
        pool->head = (pool->head + 1) % pool->queue_size;
        pool->count -= 1;

        /* Unlock */
        pthread_mutex_unlock(&(pool->lock));

        /* Get to work */
        (*(task.function))(task.argument);
    }

    pool->started--;

    pthread_mutex_unlock(&(pool->lock));
    pthread_exit(NULL);
    return(NULL);
}
