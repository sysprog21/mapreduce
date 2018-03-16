#define THREAD 8
#define QUEUE  256
#define DATASIZE (20000)

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "threadpool.h"

void is_simple(int n, void *_data)
{
    int *data = (int *) _data;
    int x = data[n];
    data[n] = 0;
    if (x < 2) return;
    for (int i = 2; i < x; i++)
        if (x % i == 0) return;
    data[n] = x;
}

void my_reduce(void *self, void *left, void *right)
{
    *((int *) left) += *((int *) right);
}

void *my_alloc_neutral(void *self)
{
    int *c = malloc(sizeof(int));
    *c = 0;
    return c;
}

void my_free(void *self, void *node)
{
    free(node);
}

void my_finish(void *self, void *node)
{
    printf("reduce result = %d\n", *(int *) node);
}

int main(int argc, char *argv[])
{
    threadpool_t *pool;

    pool = threadpool_create(THREAD, QUEUE, 0);
    fprintf(stderr, "Pool started with %d threads and "
            "queue size of %d\n", THREAD, QUEUE);

    int *data = malloc(DATASIZE * sizeof(int));
    for (int i = 0; i < DATASIZE; i++)
        data[i] = i + 1;

    threadpool_map(pool, DATASIZE, is_simple, data, 0);
    for (int i = 0; i < DATASIZE; i++)
        printf("%c", !!data[i] ? '-' : ' ');
    printf("\n");

    threadpool_reduce_t reduce = {
        .begin = data,
        .end = data + DATASIZE,
        .object_size = sizeof(*data),
        .self = NULL,
        .reduce = my_reduce,
        .reduce_alloc_neutral = my_alloc_neutral,
        .reduce_finish = my_finish,
        .reduce_free = my_free,
    };

    threadpool_reduce(pool, &reduce);

    threadpool_destroy(pool, 0);
    free(data);
    return 0;
}
