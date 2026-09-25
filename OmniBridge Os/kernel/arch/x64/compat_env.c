/* kernel/arch/x64/compat_env.c */
#include "compat_env.h"
#include "serial.h"

static int g_inited = 0;

void compat_env_init(void)
{
    if (g_inited) return;
    g_inited = 1;
    serial_printf("[COMPAT-ENV] init: PEB=%u bytes auxv=%u bytes\n",
                  (unsigned)sizeof(struct ob_peb_template),
                  (unsigned)sizeof(struct ob_auxv_template));
}

int compat_env_fill_peb(struct ob_peb_template *peb, uint64_t image_base,
                        uint64_t process_heap)
{
    if (!peb) return -1;

    uint8_t *p = (uint8_t *)peb;
    for (unsigned i = 0; i < sizeof(*peb); ++i) p[i] = 0;

    peb->image_base    = image_base;
    peb->process_heap  = process_heap;
    peb->ldr           = 0;            /* 占位：真实 LDR 表地址 */
    peb->os_major      = 10;           /* 模拟 Windows 10 */
    peb->os_minor      = 0;
    peb->build         = 19045;
    peb->_pad          = 0;
    peb->environment   = 0;
    peb->command_line  = 0;
    peb->dll_path      = 0;
    return 0;
}

int compat_env_fill_auxv(struct ob_auxv_template *auxv, uint32_t page_size,
                         uint32_t uid)
{
    if (!auxv) return -1;

    uint8_t *p = (uint8_t *)auxv;
    for (unsigned i = 0; i < sizeof(*auxv); ++i) p[i] = 0;

    uint32_t n = 0;
    auxv->entries[n].type  = OB_AT_PAGESZ;
    auxv->entries[n].value = page_size ? page_size : 4096u;
    ++n;
    auxv->entries[n].type  = OB_AT_UID;
    auxv->entries[n].value = uid;
    ++n;
    auxv->entries[n].type  = OB_AT_EUID;
    auxv->entries[n].value = uid;
    ++n;
    auxv->entries[n].type  = OB_AT_GID;
    auxv->entries[n].value = uid;
    ++n;
    auxv->entries[n].type  = OB_AT_EGID;
    auxv->entries[n].value = uid;
    ++n;
    auxv->entries[n].type  = OB_AT_CLKTCK;
    auxv->entries[n].value = 100;
    ++n;
    auxv->entries[n].type  = OB_AT_RANDOM;
    auxv->entries[n].value = 0;         /* 占位：真实随机种子地址 */
    ++n;
    auxv->entries[n].type  = OB_AT_NULL;
    auxv->entries[n].value = 0;
    ++n;

    auxv->count = n;
    auxv->_pad  = 0;
    return 0;
}

void compat_env_dump(const struct ob_peb_template *peb,
                     const struct ob_auxv_template *auxv)
{
    if (peb) {
        serial_printf("[COMPAT-ENV] PEB: image_base=0x%llx heap=0x%llx "
                      "os=%u.%u build=%u\n",
                      (unsigned long long)peb->image_base,
                      (unsigned long long)peb->process_heap,
                      (unsigned)peb->os_major,
                      (unsigned)peb->os_minor,
                      (unsigned)peb->build);
    }
    if (auxv) {
        serial_printf("[COMPAT-ENV] auxv: count=%u\n",
                      (unsigned)auxv->count);
        for (uint32_t i = 0; i < auxv->count; ++i) {
            serial_printf("[COMPAT-ENV]   AT[%u] type=%llu value=0x%llx\n",
                          (unsigned)i,
                          (unsigned long long)auxv->entries[i].type,
                          (unsigned long long)auxv->entries[i].value);
        }
    }
}