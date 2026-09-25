/*===OmniBridgeOs/kernel/arch/x64/see_whitelist.c===*/
#include "see_whitelist.h"
#include "see_policy.h"
#include "see.h"
#include "syscall.h"
#include "permission.h"
#include "serial.h"

static const struct see_wl_entry g_default_whitelist[] = {
    { SYS_OB_OpenFile,      OB_RES_FILE,   OB_ACCESS_READ,  "open-file-read" },
    { SYS_OB_ReadFile,      OB_RES_FILE,   OB_ACCESS_READ,  "read-file" },
    { SYS_OB_CloseHandle,   OB_RES_FILE,   OB_ACCESS_READ,  "close-handle" },
    { SYS_OB_VirtualAlloc,  OB_RES_MEMORY, OB_ACCESS_WRITE, "virtual-alloc" },
    { SYS_OB_VirtualFree,   OB_RES_MEMORY, OB_ACCESS_WRITE, "virtual-free" },
    { SYS_OB_GetCurrentToken, OB_RES_FILE, OB_ACCESS_READ,  "get-current-token" },
    { SYS_OB_CheckAccess,   OB_RES_FILE,   OB_ACCESS_READ,  "check-access" },
    { SYS_OB_GetProcessInfo,OB_RES_PROCESS,OB_ACCESS_READ,  "get-process-info" },
};

const struct see_wl_entry *see_whitelist_default(uint32_t *out_count)
{
    if (out_count) {
        *out_count = (uint32_t)(sizeof(g_default_whitelist) /
                                sizeof(g_default_whitelist[0]));
    }
    return g_default_whitelist;
}

int see_whitelist_install_default(struct see_instance *inst)
{
    if (!inst) return OB_EINVAL;

    uint32_t n = 0;
    const struct see_wl_entry *wl = see_whitelist_default(&n);

    for (uint32_t i = 0; i < n; ++i) {
        int rc = see_policy_add_rule(inst,
                                     wl[i].syscall_nr,
                                     wl[i].res_type,
                                     wl[i].access_mask);
        if (rc != 0) return rc;
    }

    serial_printf("[SEE-WL] installed %u default rules into sandbox %u\n",
                  (unsigned)n, (unsigned)inst->sandbox_id);
    return 0;
}