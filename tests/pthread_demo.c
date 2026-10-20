#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define NTHREADS 4
#define ITER     1000000

static long counter_unsafe = 0;                       /* shared, unprotected */
static long counter_safe   = 0;                       /* shared, mutex-protected */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct { int id; long partial; } arg_t;       /* per-thread private data */

static void *worker(void *p)
{
    arg_t *a = p;
    for (long i = 0; i < ITER; i++) {
        counter_unsafe++;                              /* DATA RACE: read-modify-write is not atomic */
        pthread_mutex_lock(&lock);
        counter_safe++;                                /* safe, but threads take turns */
        pthread_mutex_unlock(&lock);
        a->partial++;                                  /* safe with NO lock: only this thread owns it */
    }
    return NULL;
}

int main(void)
{
    pthread_t t[NTHREADS];
    arg_t args[NTHREADS];
    memset(args, 0, sizeof args);
    for (int i = 0; i < NTHREADS; i++) {
        args[i].id = i;
        int rc = pthread_create(&t[i], NULL, worker, &args[i]);
        if (rc != 0) { fprintf(stderr, "pthread_create: %s\n", strerror(rc)); return 1; }
    }
    for (int i = 0; i < NTHREADS; i++) pthread_join(t[i], NULL);

    long sum = 0;
    for (int i = 0; i < NTHREADS; i++) sum += args[i].partial;
    printf("expected %ld | unsafe %ld | mutex %ld | per-thread sum %ld\n",
           (long)NTHREADS * ITER, counter_unsafe, counter_safe, sum);
    return 0;
}
