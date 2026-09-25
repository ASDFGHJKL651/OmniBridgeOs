/*===OmniBridgeOs/kernel/arch/x64/see_whitelist.h===*/
#ifndef OMNIBRIDGE_SEE_WHITELIST_H
#define OMNIBRIDGE_SEE_WHITELIST_H

#include <stdint.h>

struct see_instance;

#define SEE_WL_MAX 64

struct see_wl_entry {
    uint64_t syscall_nr;
    uint32_t res_type;
    uint32_t access_mask;
    const char *desc;
};

const struct see_wl_entry *see_whitelist_default(uint32_t *out_count);

int see_whitelist_install_default(struct see_instance *inst);

#endif /* OMNIBRIDGE_SEE_WHITELIST_H */