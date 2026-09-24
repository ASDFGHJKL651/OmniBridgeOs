/*===OmniBridgeOs/kernel/arch/x64/obr_test.c===*/
#include "obr_test.h"
#include "obr.h"
#include "serial.h"

/* 栈上构造最小 .obr 镜像，严禁 kmalloc / pmm_* */

static int failures = 0;

static void check(const char *desc, int ok)
{
    if (ok) {
        serial_printf("[OBR-TEST] OK  : %s\n", desc);
    } else {
        serial_printf("[OBR-TEST] FAIL: %s\n", desc);
        failures++;
    }
}

/* 构造一个最小有效镜像：头部 + 1 个 LOAD 段 */
static void build_minimal(uint8_t *img, uint64_t img_size)
{
    for (uint64_t i = 0; i < img_size; ++i) img[i] = 0;

    struct obr_header *h = (struct obr_header *)img;
    h->magic      = OBR_MAGIC;
    h->version    = OBR_VERSION;
    h->arch       = OBR_ARCH_X64;
    h->min_privilege = 0;
    h->entry_point   = 0;
    h->ph_offset     = sizeof(struct obr_header);
    h->ph_count      = 1;
    h->sh_offset     = 0;
    h->sh_count      = 0;
    h->dep_count     = 0;
    h->dep_strings_offset = 0;
    h->checksum      = 0;
    h->sig_type      = 0;
    h->sig_length    = 0;

    struct obr_phdr *ph =
        (struct obr_phdr *)(img + sizeof(struct obr_header));
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

void obr_test(void)
{
    failures = 0;

    static uint8_t image[256];
    static uint8_t target[64];
    for (unsigned i = 0; i < sizeof(target); ++i) target[i] = 0xAA;

    build_minimal(image, sizeof(image));

    /* 1) 头部校验 */
    const struct obr_header *h = (const struct obr_header *)image;
    int rc = obr_validate_header(h, sizeof(image));
    check("validate_header returns 0", rc == 0);

    /* 2) 加载 */
    void *entry = 0;
    rc = obr_load(image, sizeof(image),
                  (uint64_t)(uintptr_t)target, &entry);
    check("obr_load returns 0", rc == 0);
    check("entry == target", entry == (void *)target);

    /* 3) 数据段正确性 */
    uint64_t *t64 = (uint64_t *)target;
    check("payload copied (filesz=8)",
          t64[0] == 0xDEADBEEFCAFEBABEULL);
    check("BSS zeroed (memsz=16)",
          t64[1] == 0);

    /* 4) 魔数错误 */
    image[0] = 0xFF;
    rc = obr_validate_header((const struct obr_header *)image,
                             sizeof(image));
    check("bad magic rejected", rc < 0);
    image[0] = (uint8_t)(OBR_MAGIC & 0xFF);

    /* 5) ph_count 越界 */
    struct obr_header *hm = (struct obr_header *)image;
    hm->ph_count = 0xFFFF;
    rc = obr_validate_header(hm, sizeof(image));
    check("ph_count overflow rejected", rc < 0);
    hm->ph_count = 1;

    /* 6) image_size 太小 */
    rc = obr_validate_header(hm, 4);
    check("small image rejected", rc < 0);

    if (failures == 0) {
        serial_printf("[OBR-TEST] selftest OK\n");
    } else {
        serial_printf("[OBR-TEST] selftest FAILED: %d case(s)\n",
                      failures);
    }
}
/*===OmniBridgeOs/kernel/arch/x64/obr_test.c 结束===*/