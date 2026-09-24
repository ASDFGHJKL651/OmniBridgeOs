/*===OmniBridgeOs/usr/lib/oblibc/pthread.c===*/
#include "../../include/ob/pthread.h"
#include "../../include/ob/ob.h"
#include "../../include/ob/unistd.h"
#include <stdint.h>

/*
 * TLS 使用 %fs 段。
 * 人工必须审查：
 *   - %fs 基址由内核为每个线程独立设置；内核已将 tls_base 值预填到
 *     TLS 页的第一个 8 字节，因此 %fs:0 就是 tls_block* 自身。
 *   - pthread_key_create / setspecific / getspecific 都通过 tls_self()
 *     访问。
 */

struct tls_block {
    void *specific[PTHREAD_KEYS_MAX];
    int   key_used[PTHREAD_KEYS_MAX];
    unsigned self_id;
    uint32_t _pad;
};

static inline struct tls_block *tls_self(void)
{
    struct tls_block *b;
    __asm__ __volatile__("movq %%fs:0, %0" : "=r"(b));
    return b;
}

/*
 * ★ 修复 B：把 pthread 相关的 syscall 调用隔离到 noinline 函数中。
 *
 * 人工必须审查：
 *   - ob_syscall4 是 static inline；在 -O2 下 clang 会将其内联到
 *     pthread_create / pthread_join 中。日志显示主线程首次
 *     ThreadSpawn 成功但未进入第二次循环，怀疑是内联后 clang
 *     对 syscall 的 clobber 假设与实际寄存器保存策略不一致
 *     （实际观察到：第一次成功，第二次 syscall 指令未发出）。
 *   - 使用 noinline 隔离后，pthread_create 内的控制流由编译器按
 *     常规函数调用处理，消除边缘情况。
 *   - 同时加内存屏障，保证 syscall 前后用户态内存写入对编译器可见。
 */

__attribute__((noinline))
static int64_t ob_thread_spawn_raw(uint64_t entry, uint64_t arg,
                                    uint64_t ustack_top)
{
    __asm__ __volatile__("" ::: "memory");
    int64_t r = ob_syscall3(SYS_OB_ThreadSpawn, entry, arg, ustack_top);
    __asm__ __volatile__("" ::: "memory");
    return r;
}

__attribute__((noinline))
static int64_t ob_thread_wait_raw(uint64_t tid)
{
    __asm__ __volatile__("" ::: "memory");
    int64_t r = ob_syscall1(SYS_OB_ThreadWait, tid);
    __asm__ __volatile__("" ::: "memory");
    return r;
}

__attribute__((noinline))
static void *ob_brk_raw(uint64_t want)
{
    __asm__ __volatile__("" ::: "memory");
    int64_t r = ob_syscall1(SYS_OB_Brk, want);
    __asm__ __volatile__("" ::: "memory");
    if (r < 0) return 0;
    return (void *)(uintptr_t)r;
}

int pthread_key_create(unsigned *key, void (*dtor)(void *))
{
    (void)dtor;
    if (!key) return -1;
    struct tls_block *b = tls_self();
    if (!b) return -1;
    for (int i = 0; i < PTHREAD_KEYS_MAX; ++i) {
        if (!b->key_used[i]) {
            b->key_used[i] = 1;
            *key = (unsigned)i;
            return 0;
        }
    }
    return -1;
}

int pthread_setspecific(unsigned key, const void *value)
{
    if (key >= PTHREAD_KEYS_MAX) return -1;
    struct tls_block *b = tls_self();
    if (!b) return -1;
    b->specific[key] = (void *)value;
    return 0;
}

void *pthread_getspecific(unsigned key)
{
    if (key >= PTHREAD_KEYS_MAX) return 0;
    struct tls_block *b = tls_self();
    if (!b) return 0;
    return b->specific[key];
}

/* ---------- 线程创建 / 加入 ---------- */

/*
 * ★ 任务 4 修复：显式声明 ob_brk。
 *
 * 该函数由 oblibc/stdlib.c 导出（返回 void*），但 pthread.c 未引用
 * 其头文件；C99 下隐式声明会返回 int，导致 void* 赋值类型错误。
 * 在此显式声明即可。
 *
 * 人工必须审查：
 *   - ob_brk(0) 返回当前 heap_cur（内核保证页对齐）。
 *   - ob_brk(want) 让内核从 heap_cur 起映射到 want（向上取整到页），
 *     返回旧的 heap_cur；失败返回 NULL。
 */
extern void *ob_brk(uint64_t new_brk);

/*
 * 为 pthread 线程分配 64KB 用户栈。
 *
 * 语义（人工必须审查）：
 *   - 内核 SYS_OB_Brk 已保证 heap_cur 始终页对齐；
 *     本函数再显式向上取整一次，保证传入 want 也是页对齐。
 *   - 每次调用 ob_brk(0) 返回的 heap_cur 都是新的 64KB 对齐边界，
 *     因此每个线程获得互不重叠的 [base, base+64KB) 栈区间。
 *   - 栈顶返回 base + 64KB，16 字节对齐，供 user_thread_spawn
 *     计算 stack_bottom = top - USER_STACK_SIZE 使用。
 *
 * 人工必须审查：
 *   - 本函数不再依赖 s_thread_stack_cursor；只用 ob_brk 单调推进，
 *     每个线程的栈区互不重叠。
 *   - 若内存耗尽（heap_end 到达），ob_brk 返回 0，本函数返回 0，
 *     pthread_create 返回 -1。
 */
__attribute__((noinline))
static uint64_t ob_alloc_thread_stack(void)
{
    const uint64_t STRIDE = 0x0000000000010000ULL;  /* 64 KB */
    const uint64_t PAGE   = 0x0000000000001000ULL;

    /* ★ 修复 B：使用 noinline 的 ob_brk_raw */
    void *base_raw = ob_brk_raw(0);
    if (!base_raw) return 0;

    uint64_t base = (uint64_t)(uintptr_t)base_raw;
    base = (base + PAGE - 1) & ~(PAGE - 1);

    uint64_t want = base + STRIDE;

    void *r = ob_brk_raw(want);
    if (!r) return 0;

    uint64_t top = base + STRIDE;
    top &= ~0xFULL;
    return top;
}

int pthread_create(pthread_t *th, const pthread_attr_t *attr,
                   void *(*fn)(void *), void *arg)
{
    (void)attr;
    if (!fn) return -1;

    uint64_t ustack_top = ob_alloc_thread_stack();
    if (!ustack_top) return -1;

    int64_t tid = ob_thread_spawn_raw((uint64_t)(uintptr_t)fn,
                                       (uint64_t)(uintptr_t)arg,
                                       ustack_top);
    if (tid <= 0) return -1;    /* ★ 修复 B：tid 必须 > 0 */

    if (th) *th = (pthread_t)tid;
    return 0;
}

int pthread_join(pthread_t th, void **retval)
{
    int64_t r = ob_thread_wait_raw((uint64_t)th);
    if (r < 0) return -1;
    if (retval) *retval = (void *)(uintptr_t)r;
    return 0;
}

void pthread_exit(void *retval)
{
    (void)retval;
    ob_syscall1(SYS_OB_UserExit, 0);
    for (;;) __asm__ __volatile__("hlt");
}

pthread_t pthread_self(void)
{
    return (pthread_t)ob_syscall0(SYS_OB_GetTid);
}

/* ---------- 互斥量（用户态自旋 + futex 简化） ---------- */

int pthread_mutex_init(pthread_mutex_t *m, const void *attr)
{
    (void)attr;
    if (!m) return -1;
    m->lock = 0; m->waiters = 0; m->_pad = 0;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
    if (!m) return -1;
    for (;;) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&m->lock, &expected, 1,
                                        0, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) return 0;
        __atomic_add_fetch(&m->waiters, 1, __ATOMIC_RELAXED);
        ob_syscall3(SYS_OB_FutexWait, (uint64_t)(uintptr_t)&m->lock,
                    1, 100000);
        __atomic_sub_fetch(&m->waiters, 1, __ATOMIC_RELAXED);
    }
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    if (!m) return -1;
    __atomic_store_n(&m->lock, 0, __ATOMIC_RELEASE);
    if (__atomic_load_n(&m->waiters, __ATOMIC_RELAXED) > 0) {
        ob_syscall2(SYS_OB_FutexWake, (uint64_t)(uintptr_t)&m->lock, 1);
    }
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    (void)m;
    return 0;
}

/* ---------- 条件变量 ---------- */

int pthread_cond_init(pthread_cond_t *c, const void *attr)
{
    (void)attr;
    if (!c) return -1;
    c->seq = 0; c->_pad = 0; c->mutex = 0;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    if (!c || !m) return -1;
    int seq = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
    pthread_mutex_unlock(m);
    ob_syscall3(SYS_OB_FutexWait, (uint64_t)(uintptr_t)&c->seq,
                (uint64_t)seq, 100000);
    pthread_mutex_lock(m);
    return 0;
}

int pthread_cond_signal(pthread_cond_t *c)
{
    if (!c) return -1;
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_RELEASE);
    ob_syscall2(SYS_OB_FutexWake, (uint64_t)(uintptr_t)&c->seq, 1);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c)
{
    if (!c) return -1;
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_RELEASE);
    ob_syscall2(SYS_OB_FutexWake, (uint64_t)(uintptr_t)&c->seq, 0xFFFF);
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }
/*===OmniBridgeOs/usr/lib/oblibc/pthread.c 结束===*/