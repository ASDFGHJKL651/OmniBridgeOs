/*===OmniBridgeOs/kernel/arch/x64/user/usercopy.c===*/
#include "user.h"
#include "vmm.h"
#include "serial.h"

/*
 * 用户态内存拷贝。
 *
 * 人工必须审查：
 *   - 所有 vaddr 必须经过 user_range_ok() 检查。
 *   - 必须在访问前 stac()（SMAP），访问后 clac()。
 *   - 页错误（#PF）若由用户地址触发，应回滚；本步在头文件中
 *     约定：调用者需提供 fallback 路径，本模块不做异常捕获。
 */

int user_range_ok_strict(uint64_t vaddr, uint64_t size)
{
    if (!user_range_ok(vaddr, size)) return 0;
    /* TODO：将来逐页校验 PTE 存在且 PTE_USER=1 */
    return 1;
}

/*
 * copy_from_user / copy_to_user 的简化版本：
 * 不做按页 PTE 检查（第 18C 步 PTE 由 user_setup 一次性建好且
 * 所有映射都在 [USER_STACK_BOTTOM, USER_STACK_TOP) 与 TLS 页，
 * 调用方只需保证 vaddr 落在用户空间即可）。
 */
int copy_from_user(void *dst, uint64_t src_uaddr, uint64_t len)
{
    if (!dst) return -1;
    if (!user_range_ok_strict(src_uaddr, len)) return -1;

    stac();
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)(uintptr_t)src_uaddr;
    for (uint64_t i = 0; i < len; ++i) d[i] = s[i];
    clac();
    return 0;
}

int copy_to_user(uint64_t dst_uaddr, const void *src, uint64_t len)
{
    if (!src) return -1;
    if (!user_range_ok_strict(dst_uaddr, len)) return -1;

    stac();
    uint8_t *d = (uint8_t *)(uintptr_t)dst_uaddr;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < len; ++i) d[i] = s[i];
    clac();
    return 0;
}

int strnlen_user(uint64_t uaddr, uint64_t max)
{
    if (!user_range_ok(uaddr, 1)) return -1;
    uint64_t n = 0;
    stac();
    const char *s = (const char *)(uintptr_t)uaddr;
    while (n < max && s[n] != '\0') ++n;
    clac();
    return (int)n;
}

int strncpy_from_user(char *dst, uint64_t uaddr, uint64_t max)
{
    if (!dst) return -1;
    if (!user_range_ok(uaddr, 1)) return -1;
    uint64_t i = 0;
    stac();
    const char *s = (const char *)(uintptr_t)uaddr;
    while (i + 1 < max && s[i] != '\0') { dst[i] = s[i]; ++i; }
    dst[i] = '\0';
    clac();
    return (int)i;
}
/*===OmniBridgeOs/kernel/arch/x64/user/usercopy.c 结束===*/