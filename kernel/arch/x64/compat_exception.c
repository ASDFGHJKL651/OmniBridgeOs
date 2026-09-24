/* kernel/arch/x64/compat_exception.c */
#include "compat_exception.h"
#include "compat_object.h"
#include "serial.h"

/* 读取 CR2 */
static uint64_t read_cr2(void)
{
    uint64_t v;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(v));
    return v;
}

int compat_exception_handle(struct regs *r, struct task_t *cur)
{
    if (!r || !cur) return 0;
    if (cur->compat_type == COMPAT_TYPE_NONE) return 0;

    uint64_t cr2 = 0;
    if (r->vector == 14) {
        cr2 = read_cr2();
    }

    struct compat_signal_info info;
    if (compat_signal_translate(r->vector, r->error, cr2, r->rip, &info) != 0) {
        return 0;
    }

    serial_printf("[COMPAT] exception: vec=%llu sig=%u rip=0x%llx "
                  "cr2=0x%llx\n",
                  (unsigned long long)r->vector,
                  (unsigned)info.signo,
                  (unsigned long long)r->rip,
                  (unsigned long long)cr2);

    compat_signal_dispatch(cur, &info);
    return 1;
}