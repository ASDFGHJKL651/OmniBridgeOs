/* kernel/arch/x64/compat_object.c */
#include "compat_object.h"
#include "audit.h"
#include "serial.h"

static int g_inited = 0;

void compat_object_init(void)
{
    if (g_inited) return;
    g_inited = 1;
    serial_printf("[COMPAT-OBJECT] init\n");
}

int compat_signal_translate(uint64_t vector, uint64_t error,
                            uint64_t cr2, uint64_t rip,
                            struct compat_signal_info *out)
{
    if (!out) return -1;

    out->signo      = OB_SIGTERM;
    out->code       = (uint32_t)error;
    out->fault_addr = cr2;
    out->rip        = rip;

    switch (vector) {
    case 0:  /* #DE 除零 */
        out->signo = OB_SIGFPE;
        break;
    case 6:  /* #UD 无效指令 */
        out->signo = OB_SIGILL;
        break;
    case 13: /* #GP */
        out->signo = OB_SIGSEGV;
        break;
    case 14: /* #PF */
        out->signo = OB_SIGSEGV;
        break;
    case 17: /* #AC 对齐 */
        out->signo = OB_SIGBUS;
        break;
    default:
        out->signo = OB_SIGTERM;
        break;
    }
    return 0;
}

int compat_signal_dispatch(struct task_t *cur,
                           const struct compat_signal_info *info)
{
    if (!info) return -1;

    uint64_t pid = cur ? cur->pid : 0;
    uint32_t ctype = cur ? cur->compat_type : 0;

    /* 审计（CRITICAL） */
    audit_event(AUDIT_EV_COMPAT_EXCEPTION, AUDIT_LVL_CRITICAL,
                pid, info->signo, info->rip, info->code, "(compat-exception)");

    if (ctype == COMPAT_TYPE_WIN32) {
        serial_printf("[COMPAT] SEH dispatch sig=%u\n",
                      (unsigned)info->signo);
    } else if (ctype == COMPAT_TYPE_LINUX) {
        serial_printf("[COMPAT] signal dispatch sig=%u\n",
                      (unsigned)info->signo);
    } else {
        serial_printf("[COMPAT] dispatch (no compat type) sig=%u\n",
                      (unsigned)info->signo);
    }

    return 0;
}

void compat_object_dump(void)
{
    serial_printf("[COMPAT-OBJECT] signals: SEGV=%u FPE=%u ILL=%u BUS=%u "
                  "TERM=%u KILL=%u\n",
                  (unsigned)OB_SIGSEGV, (unsigned)OB_SIGFPE,
                  (unsigned)OB_SIGILL, (unsigned)OB_SIGBUS,
                  (unsigned)OB_SIGTERM, (unsigned)OB_SIGKILL);
}