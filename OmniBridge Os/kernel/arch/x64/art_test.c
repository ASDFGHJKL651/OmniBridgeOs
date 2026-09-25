/*===OmniBridgeOs/kernel/arch/x64/art_test.c===*/
#include "art_test.h"
#include "art.h"
#include "task.h"
#include "serial.h"

static int failures = 0;

static void check(const char *desc, int ok)
{
    if (ok) {
        serial_printf("[ART-TEST] OK  : %s\n", desc);
    } else {
        serial_printf("[ART-TEST] FAIL: %s\n", desc);
        failures++;
    }
}

static int g_walk_count = 0;

static void walk_cb(const struct art_entry *e, void *arg)
{
    (void)e; (void)arg;
    g_walk_count++;
}

void art_test(void)
{
    failures = 0;
    serial_printf("[ART-TEST] === begin ===\n");

    /* 1) 初始化 */
    art_init();
    /* art_init 是幂等的；可能之前 compat_preload 已注册条目。
     * 因此这里只做非负验证，然后用 art_count 作为后续比较基线。 */
    check("art_init count >= 0", art_count() >= 0);

    uint32_t before = art_count();

    /* 2) 注册 */
    int rc = art_register("ob_art_test_api", (void *)0x1234, 0x01);
    check("register increases count",
          rc == 0 && art_count() == before + 1);

    /* 3) lookup 存在 */
    struct art_entry e;
    rc = art_lookup("ob_art_test_api", &e);
    check("lookup existing succeeds",
          rc == 0 && e.func_ptr == (void *)0x1234 && e.flags == 0x01);

    /* 4) lookup 不存在 */
    rc = art_lookup("ob_art_missing_api", &e);
    check("lookup missing returns -ENOENT", rc == OB_ENOENT);

    /* 5) 重复注册（更新） */
    rc = art_register("ob_art_test_api", (void *)0x5678, 0x02);
    check("re-register same name updates",
          rc == 0 && art_count() == before + 1);

    rc = art_lookup("ob_art_test_api", &e);
    check("re-register updates func_ptr",
          rc == 0 && e.func_ptr == (void *)0x5678 && e.flags == 0x02);

    /* 6) walk */
    g_walk_count = 0;
    art_walk(walk_cb, 0);
    check("walk visits all entries", g_walk_count == (int)art_count());

    /* 7) 名称过长拒绝 */
    char longname[ART_NAME_MAX + 4];
    for (unsigned i = 0; i < sizeof(longname) - 1; ++i) longname[i] = 'a';
    longname[sizeof(longname) - 1] = '\0';
    rc = art_register(longname, 0, 0);
    check("overlong name rejected", rc == OB_EINVAL);

    if (failures == 0) {
        serial_printf("[ART-TEST] selftest OK\n");
    } else {
        serial_printf("[ART-TEST] selftest FAILED: %d case(s)\n", failures);
    }
    serial_printf("[ART-TEST] === end ===\n");
}
/*===OmniBridgeOs/kernel/arch/x64/art_test.c 结束===*/