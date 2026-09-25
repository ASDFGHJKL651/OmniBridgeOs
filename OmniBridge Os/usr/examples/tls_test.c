/*===OmniBridgeOs/usr/examples/tls_test.c===*/
/*
 * tls_test.c —— pthread TLS 验收。
 *
 * 验收对应（第 18C 步）：
 *   ✓ pthread 的 TLS 基址（%fs）设置与切换。
 *   ✓ pthread_key_create / setspecific / getspecific 工作正常。
 *
 * 预期输出（串口）：
 *   [tls_test] key created
 *   [tls_test] main thread set/get ok
 *   [tls_test] worker thread set/get ok
 *   [tls_test] main thread value preserved
 *   [tls_test] OK
 */
#include "../include/ob/ob.h"
#include "../include/ob/stdio.h"
#include "../include/ob/pthread.h"
#include <stdint.h>

static unsigned g_key = 0;
static volatile int g_worker_ok = 0;

static void *worker(void *arg)
{
    (void)arg;
    char *msg = "worker-local";
    pthread_setspecific(g_key, msg);

    char *got = (char *)pthread_getspecific(g_key);
    if (got && got[0] == 'w') g_worker_ok = 1;
    else                        g_worker_ok = 0;

    printf("[tls_test] worker thread set/get %s\n",
           g_worker_ok ? "ok" : "FAIL");

    /* ★ 修复：经 uintptr_t 中转，避免 int→void* 的直接转换警告。 */
    return (void *)(uintptr_t)(unsigned)g_worker_ok;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    if (pthread_key_create(&g_key, 0) != 0) {
        printf("[tls_test] FAIL: pthread_key_create\n");
        return 1;
    }
    printf("[tls_test] key created\n");

    char *main_val = "main-local";
    pthread_setspecific(g_key, main_val);
    char *got = (char *)pthread_getspecific(g_key);
    if (!got || got[0] != 'm') {
        printf("[tls_test] FAIL: main set/get\n");
        return 1;
    }
    printf("[tls_test] main thread set/get ok\n");

    pthread_t th;
    if (pthread_create(&th, 0, worker, 0) != 0) {
        printf("[tls_test] FAIL: pthread_create\n");
        return 1;
    }
    void *ret = 0;
    pthread_join(th, &ret);

    if (!g_worker_ok) {
        printf("[tls_test] FAIL: worker TLS not isolated\n");
        return 1;
    }

    /* main 线程的值不得被 worker 覆盖 */
    got = (char *)pthread_getspecific(g_key);
    if (!got || got[0] != 'm') {
        printf("[tls_test] FAIL: main value polluted\n");
        return 1;
    }
    printf("[tls_test] main thread value preserved\n");

    printf("[tls_test] OK\n");
    return 0;
}
/*===OmniBridgeOs/usr/examples/tls_test.c 结束===*/