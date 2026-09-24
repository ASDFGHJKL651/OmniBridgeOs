#include "obinit_test.h"
#include "obinit.h"
#include "kmalloc.h"
#include "pmm.h"
#include "serial.h"

/*
 * obinit.toml 解析器自检（第 12 步）。
 * 严禁调用 task_create；使用栈上/static 缓冲区。
 *
 * ★ 修复说明（人工必须审查）：
 *   1) 早期版本用 `const char *text = "..."` 搭配 `SLIT(text)`。
 *      `sizeof(指针)` 恒为 8，导致宏只得出长度 7，解析器只能看到
 *      `[servic`（没有 ']'），所有"合法输入"测试均被解析器当作
 *      非法输入拒绝。
 *   2) 现在改用 `static const char text[] = "..."`，`sizeof(text)` 为
 *      完整字面量长度（含末尾 '\0'），`sizeof(text) - 1` 即真实长度。
 *   3) 保留 `SLIT` 宏供未来在字面量上直接使用；但在变量上使用时，
 *      变量必须是数组而非指针。
 */

#define SLIT(s) ((uint64_t)(sizeof(s) - 1))

static int failures = 0;

static void check(const char *desc, int ok)
{
    if (ok) {
        serial_printf("[OBINIT-TEST] OK  : %s\n", desc);
    } else {
        serial_printf("[OBINIT-TEST] FAIL: %s\n", desc);
        failures++;
    }
}

/* 预热 obinit_service 相关的 slab 缓存，避免"无泄漏"判据假阳性。 */
static void warmup(void)
{
    void *p = kmalloc(512);   /* obinit_service 约 352 字节，落在 kmalloc-512 */
    if (p) kfree(p);
    p = kmalloc(256);
    if (p) kfree(p);
}

void obinit_test(void)
{
    failures = 0;

    serial_printf("[OBINIT-TEST] === begin ===\n");

    warmup();
    uint64_t free_before = pmm_free_pages_count();

    /* ---------- 1) 单服务 ---------- */
    {
        static const char text[] =
            "[service.a]\n"
            "path = \"/system/critical/a.obr\"\n"
            "privilege = 7\n"
            "pid_hint = 10\n"
            "critical = true\n";

        struct obinit_config cfg;
        int rc = obinit_parse(text, SLIT(text), &cfg);
        check("parse single", rc == 0);
        if (rc == 0) {
            check("count == 1", cfg.count == 1);
            if (cfg.head) {
                check("name == 'a'",
                      cfg.head->name[0] == 'a' && cfg.head->name[1] == '\0');
                check("path starts with /",
                      cfg.head->path[0] == '/');
                check("privilege == 7", cfg.head->privilege == 7);
                check("pid_hint == 10",  cfg.head->pid_hint == 10);
                check("critical == 1",   cfg.head->critical == 1);
                check("restart == 0",    cfg.head->restart == 0);
            }
            obinit_free(&cfg);
        }
    }

    /* ---------- 2) 多服务：顺序 a -> b -> c ---------- */
    {
        static const char text[] =
            "[service.a]\n"
            "path = \"/a\"\n"
            "[service.b]\n"
            "path = \"/b\"\n"
            "privilege = 3\n"
            "[service.c]\n"
            "path = \"/c\"\n";

        struct obinit_config cfg;
        int rc = obinit_parse(text, SLIT(text), &cfg);
        check("parse multi", rc == 0);
        if (rc == 0) {
            check("count == 3", cfg.count == 3);
            if (cfg.head && cfg.head->next && cfg.head->next->next) {
                check("order a->b->c",
                      cfg.head->name[0] == 'a' &&
                      cfg.head->next->name[0] == 'b' &&
                      cfg.head->next->next->name[0] == 'c');
                check("b.privilege == 3",
                      cfg.head->next->privilege == 3);
                check("c.path == \"/c\"",
                      cfg.head->next->next->path[0] == '/' &&
                      cfg.head->next->next->path[1] == 'c' &&
                      cfg.head->next->next->path[2] == '\0');
            }
            obinit_free(&cfg);
        }
    }

    /* ---------- 3) 非法输入 ---------- */

    /* 缺 path */
    {
        static const char text[] = "[service.x]\nprivilege = 5\n";
        struct obinit_config cfg;
        int rc = obinit_parse(text, SLIT(text), &cfg);
        check("missing path -> -1", rc == -1);
    }

    /* privilege = 99 */
    {
        static const char text[] =
            "[service.x]\npath = \"/x\"\nprivilege = 99\n";
        struct obinit_config cfg;
        int rc = obinit_parse(text, SLIT(text), &cfg);
        check("privilege=99 -> -1", rc == -1);
    }

    /* pid_hint = abc */
    {
        static const char text[] =
            "[service.x]\npath = \"/x\"\npid_hint = abc\n";
        struct obinit_config cfg;
        int rc = obinit_parse(text, SLIT(text), &cfg);
        check("pid_hint=abc -> -1", rc == -1);
    }

    /* path 缺引号 */
    {
        static const char text[] =
            "[service.x]\npath = 123\n";
        struct obinit_config cfg;
        int rc = obinit_parse(text, SLIT(text), &cfg);
        check("path without quotes -> -1", rc == -1);
    }

    /* ---------- 4) 空文本 ---------- */
    {
        struct obinit_config cfg;
        int rc = obinit_parse("", 0, &cfg);
        check("empty text -> 0, count 0",
              rc == 0 && cfg.count == 0);
        obinit_free(&cfg);
    }

    /* ---------- 5) 无泄漏（预热后，各轮 kmalloc/kfree 净变化为 0） ---------- */
    uint64_t free_after = pmm_free_pages_count();
    check("no page leak", free_after == free_before);

    if (failures == 0) {
        serial_printf("[OBINIT-TEST] selftest OK\n");
    } else {
        serial_printf("[OBINIT-TEST] selftest FAILED: %d case(s)\n",
                      failures);
    }
    serial_printf("[OBINIT-TEST] === end ===\n");
}