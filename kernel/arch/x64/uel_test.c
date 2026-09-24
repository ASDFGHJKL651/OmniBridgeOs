#include "uel_test.h"
#include "uel.h"
#include "obr.h"
#include "task.h"
#include "serial.h"

/*
 * UEL 自检（第 13 步）。
 *
 * 约束：
 *   - 严禁调用 task_create / kmalloc / pmm_*；
 *   - 使用栈上 fake task；
 *   - 不真正跳转 entry。
 */

static int failures = 0;

static void check(const char *desc, int ok)
{
    if (ok) {
        serial_printf("[UEL-TEST] OK  : %s\n", desc);
    } else {
        serial_printf("[UEL-TEST] FAIL: %s\n", desc);
        failures++;
    }
}

/* 在栈上构造最小 .obr：头部 + 1 个 LOAD + 8 字节 payload */
static void build_minimal_obr(uint8_t *img, uint64_t img_size,
                              uint8_t min_priv)
{
    for (uint64_t i = 0; i < img_size; ++i) img[i] = 0;

    struct obr_header *h = (struct obr_header *)img;
    h->magic          = OBR_MAGIC;
    h->version        = OBR_VERSION;
    h->arch           = OBR_ARCH_X64;
    h->min_privilege  = min_priv;
    h->entry_point    = sizeof(struct obr_header) +
                        sizeof(struct obr_phdr);
    h->ph_offset      = sizeof(struct obr_header);
    h->ph_count       = 1;
    h->sh_offset      = 0;
    h->sh_count       = 0;
    h->dep_count      = 0;
    h->dep_strings_offset = 0;
    h->checksum       = 0;
    h->sig_type       = 0;
    h->sig_length     = 0;

    struct obr_phdr *ph = (struct obr_phdr *)
        (img + sizeof(struct obr_header));
    ph->type   = OBR_PT_LOAD;
    ph->flags  = OBR_PF_R | OBR_PF_X;
    ph->offset = sizeof(struct obr_header) + sizeof(struct obr_phdr);
    ph->vaddr  = 0;
    ph->filesz = 8;
    ph->memsz  = 16;
    ph->align  = 0;

    uint64_t *payload = (uint64_t *)(img + ph->offset);
    *payload = 0xDEADBEEFCAFEBABEULL;
}

static void make_fake(struct task_t *t, uint64_t pid, uint8_t priv)
{
    uint8_t *p = (uint8_t *)t;
    for (unsigned i = 0; i < sizeof(*t); ++i) p[i] = 0;
    t->pid             = pid;
    t->privilege_level = priv;
    t->security_token.level = priv;
}

void uel_test(void)
{
    failures = 0;
    serial_printf("[UEL-TEST] === begin ===\n");

    /* ---------- 1) 格式识别 ---------- */
    {
        uint8_t empty[4] = {0, 0, 0, 0};
        check("detect empty == unknown",
              uel_detect_format(empty, 4) == UEL_FMT_UNKNOWN);

        uint8_t obr[4] = { 0x20, 0x52, 0x42, 0x4F };  /* LE(0x4F425220) */
        check("detect OBR",
              uel_detect_format(obr, 4) == UEL_FMT_OBR);

        uint8_t pe[4] = { 'M', 'Z', 0x90, 0x00 };
        check("detect PE",
              uel_detect_format(pe, 4) == UEL_FMT_PE);

        uint8_t elf[4] = { 0x7F, 'E', 'L', 'F' };
        check("detect ELF",
              uel_detect_format(elf, 4) == UEL_FMT_ELF);
    }

    /* ---------- 2) 加载最小 .obr ---------- */
    static uint8_t image[256];
    static uint8_t target[64];
    for (unsigned i = 0; i < sizeof(target); ++i) target[i] = 0xAA;

    build_minimal_obr(image, sizeof(image), 0);

    struct uel_load_result res;
    uint64_t load_base = (uint64_t)(uintptr_t)target;

    int rc = uel_load(image, sizeof(image), load_base, 0, &res);
    check("load minimal obr", rc == 0);
    if (rc == 0) {
        check("format == OBR", res.format == UEL_FMT_OBR);
        check("entry == load_base + entry_point",
              res.entry == load_base + sizeof(struct obr_header)
                                     + sizeof(struct obr_phdr));

        const uint64_t *t64 = (const uint64_t *)target;
        check("payload copied", t64[0] == 0xDEADBEEFCAFEBABEULL);
        check("bss zeroed",     t64[1] == 0);
    }

    /* ---------- 3) min_privilege 预检 ---------- */
    {
        build_minimal_obr(image, sizeof(image), 7);
        struct task_t fake;
        make_fake(&fake, 2000, 5);

        struct uel_load_result r2;
        int rc2 = uel_load(image, sizeof(image),
                           (uint64_t)(uintptr_t)target, &fake, &r2);
        check("min_privilege denied (rc=-1)", rc2 == OB_EPERM);

        /* 内核引导（caller==NULL）应允许 */
        rc2 = uel_load(image, sizeof(image),
                       (uint64_t)(uintptr_t)target, 0, &r2);
        check("min_privilege allowed for kernel bootstrap", rc2 == 0);
    }

    /* ---------- 4) 边界 ---------- */
    {
        /* ph_count 溢出 */
        build_minimal_obr(image, sizeof(image), 0);
        struct obr_header *hm = (struct obr_header *)image;
        hm->ph_count = 0xFFFF;
        struct uel_load_result r;
        int rc2 = uel_load(image, sizeof(image),
                           (uint64_t)(uintptr_t)target, 0, &r);
        check("ph_count overflow rejected", rc2 != 0);
    }

    {
        /* image_size < sizeof(obr_header) */
        struct uel_load_result r;
        int rc2 = uel_load(image, 4,
                           (uint64_t)(uintptr_t)target, 0, &r);
        check("small image rejected", rc2 != 0);
    }

    {
        /* filesz > memsz */
        build_minimal_obr(image, sizeof(image), 0);
        struct obr_phdr *ph = (struct obr_phdr *)
            (image + sizeof(struct obr_header));
        ph->filesz = 32;
        ph->memsz  = 8;
        struct uel_load_result r;
        int rc2 = uel_load(image, sizeof(image),
                           (uint64_t)(uintptr_t)target, 0, &r);
        check("filesz > memsz rejected", rc2 != 0);
    }

    {
        /* load_base 越界（回绕） */
        build_minimal_obr(image, sizeof(image), 0);
        struct uel_load_result r;
        uint64_t bad_base = 0xFFFFFFFFFFFFFFF0ULL;
        int rc2 = uel_load(image, sizeof(image), bad_base, 0, &r);
        check("load_base overflow rejected", rc2 != 0);
    }

    if (failures == 0) {
        serial_printf("[UEL-TEST] selftest OK\n");
    } else {
        serial_printf("[UEL-TEST] selftest FAILED: %d case(s)\n", failures);
    }
    serial_printf("[UEL-TEST] === end ===\n");
}