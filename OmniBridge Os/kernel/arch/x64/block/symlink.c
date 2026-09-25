/*===OmniBridgeOs/kernel/arch/x64/block/symlink.c===*/
#include "symlink.h"
#include "serial.h"

#ifndef OB_ELOOP
#define OB_ELOOP (-40)
#endif

void symlink_init(void)
{
    serial_printf("[SYMLINK] init: max_target=%u\n",
                  (unsigned)SYMLINK_MAX_TARGET);
}

static int str_starts_with(const char *s, const char *pfx)
{
    while (*pfx) {
        if (*s != *pfx) return 0;
        ++s; ++pfx;
    }
    return 1;
}

/*
 * 判定 target 是否命中内核保护目录。
 *
 * 索引说明（人工必须审查——这里是 off-by-one 的经典陷阱）：
 *
 *   "/kernel"           长度 7，索引 0..6，末字符 'l' 在 6，
 *                       因此下一个字符是 target[7]。
 *
 *   "/system/kernel"    长度 14，索引 0..13，末字符 'l' 在 13，
 *                       因此下一个字符是 target[14]。
 *
 *   "/system/critical"  长度 16，索引 0..15，末字符 'l' 在 15，
 *                       因此下一个字符是 target[16]。
 *
 * 命中条件：下一个字符为 '\0'（路径恰好是保护目录自身）
 *           或 '/'（保护目录的子项）。
 *
 * 任何索引错位都会导致保护失效（过短 → 漏判；过长 → 越界），
 * 必须严格按字符串字面量长度核算。
 */
int symlink_check_target(const char *target)
{
    if (!target) return -22;

    if (str_starts_with(target, "/kernel") &&
        (target[7] == '\0' || target[7] == '/'))
        return -1;

    if (str_starts_with(target, "/system/kernel") &&
        (target[14] == '\0' || target[14] == '/'))
        return -1;

    if (str_starts_with(target, "/system/critical") &&
        (target[16] == '\0' || target[16] == '/'))
        return -1;

    return 0;
}

int symlink_resolve(const char *link_target, const char *base_dir,
                    char *out, uint32_t out_size)
{
    if (!link_target || !out || out_size < 2) return -22;

    if (symlink_check_target(link_target) != 0) return -1;

    if (link_target[0] == '/') {
        /* 绝对路径：原样复制 */
        uint32_t i = 0;
        while (link_target[i] && i + 1 < out_size) {
            out[i] = link_target[i];
            ++i;
        }
        out[i] = '\0';
        return 0;
    }

    /* 相对路径：拼接 base_dir + "/" + link_target */
    if (!base_dir) base_dir = "/";
    uint32_t p = 0;
    while (base_dir[p] && p + 1 < out_size) { out[p] = base_dir[p]; ++p; }

    /* 去掉 base_dir 末尾的冗余 '/'（除非 base_dir 就是 "/"） */
    if (p > 1 && out[p - 1] == '/') --p;

    /* 补一个 '/' */
    if (p + 1 < out_size) out[p++] = '/';

    uint32_t i = 0;
    while (link_target[i] && p + 1 < out_size) {
        out[p++] = link_target[i++];
    }
    out[p] = '\0';
    return 0;
}
/*===OmniBridgeOs/kernel/arch/x64/block/symlink.c 结束===*/