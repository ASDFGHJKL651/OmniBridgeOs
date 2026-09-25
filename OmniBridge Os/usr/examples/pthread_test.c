#include "../include/ob/ob.h"
#include "../include/ob/stdio.h"
#include "../include/ob/pthread.h"

static int g_counter = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void *worker(void *arg)
{
    int id = (int)(long)arg;
    for (int i = 0; i < 100; ++i) {
        pthread_mutex_lock(&g_lock);
        g_counter++;
        pthread_mutex_unlock(&g_lock);
    }
    printf("[thread %d] done\n", id);
    return (void *)(long)(id * 10);   /* 返回值 = id*10 */
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    printf("pthread_test start\n");

    pthread_t t[3];
    for (int i = 0; i < 3; ++i) {
        if (pthread_create(&t[i], 0, worker, (void *)(long)i) != 0) {
            printf("pthread_create %d failed\n", i);
            return 1;
        }
    }

    int fail = 0;
    for (int i = 0; i < 3; ++i) {
        void *ret = 0;
        pthread_join(t[i], &ret);
        printf("[join %d] retval=%ld\n", i, (long)ret);

        /* ★ 第 18D 步：断言 join 返回值为 id*10 */
        if ((long)ret != i * 10) {
            printf("pthread_test FAIL: join %d retval mismatch "
                   "(want %d, got %ld)\n", i, i * 10, (long)ret);
            fail = 1;
        }
    }

    if (g_counter != 300) {
        printf("pthread_test FAIL: counter=%d (expect 300)\n", g_counter);
        fail = 1;
    }

    if (fail) return 1;

    printf("counter=%d (expect 300)\n", g_counter);
    printf("pthread_test OK\n");
    return 0;
}