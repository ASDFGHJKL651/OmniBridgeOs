/*===OmniBridgeOs/tests/host/test_pthread_logic.c===*/
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>

static int fail = 0;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fail++; } } while (0)

static int counter = 0;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

static void *worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 10000; ++i) {
        pthread_mutex_lock(&m);
        counter++;
        pthread_mutex_unlock(&m);
    }
    return 0;
}

int main(void)
{
    pthread_t t[4];
    for (int i = 0; i < 4; ++i)
        CHECK(pthread_create(&t[i], 0, worker, 0) == 0);
    for (int i = 0; i < 4; ++i)
        CHECK(pthread_join(t[i], 0) == 0);
    CHECK(counter == 40000);

    if (fail == 0) { printf("test_pthread_logic: OK\n"); return 0; }
    printf("test_pthread_logic: %d failures\n", fail);
    return 1;
}
/*===OmniBridgeOs/tests/host/test_pthread_logic.c 结束===*/