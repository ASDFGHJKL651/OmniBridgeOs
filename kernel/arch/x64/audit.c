/*===OmniBridgeOs/kernel/arch/x64/audit.c===*/
#include "audit.h"
#include "serial.h"
#include "spinlock.h"

/*
 * 环形缓冲状态。
 *
 * 说明：单核阶段用 cli/sti 保护所有读写；SMP 阶段（步骤 31）改为
 * 每 CPU 环形缓冲 + 全局汇聚。
 */
static struct audit_entry g_ring[AUDIT_RING_SIZE];
static uint64_t g_write_idx    = 0;   /* 累计写入次数，用于定位槽位 */
static uint64_t g_total_count  = 0;   /* 累计记录条数 */
static uint64_t g_current_cnt  = 0;   /* 当前有效条目数（<= RING_SIZE） */

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static const char *level_name(uint32_t lvl)
{
    switch (lvl) {
    case AUDIT_LVL_INFO:     return "INFO";
    case AUDIT_LVL_WARN:     return "WARN";
    case AUDIT_LVL_CRITICAL: return "CRITICAL";
    default:                 return "?";
    }
}

static const char *type_name(uint32_t type)
{
    switch (type) {
    case AUDIT_EV_PID_VIOLATION:    return "SEC_PID_VIOLATION";
    case AUDIT_EV_KERNEL_MEM_WRITE: return "SEC_KERNEL_MEM_WRITE";
    case AUDIT_EV_KERNEL_MEM_READ:  return "SEC_KERNEL_MEM_READ";
    case AUDIT_EV_CRITICAL_ACCESS:  return "SEC_CRITICAL_ACCESS";
    case AUDIT_EV_SANDBOX_ESCAPE:   return "SEC_SANDBOX_ESCAPE";
    case AUDIT_EV_ITA_FAILURE:      return "SEC_ITA_FAILURE";
    /* ★ 第 18 步 */
    case AUDIT_EV_COMPAT_PATH_REDIRECT: return "COMPAT_PATH_REDIRECT";
    case AUDIT_EV_COMPAT_ACCESS_DENIED: return "COMPAT_ACCESS_DENIED";
    case AUDIT_EV_COMPAT_EXCEPTION:     return "COMPAT_EXCEPTION";
    default:                        return "SEC_UNKNOWN";
    }
}

void audit_init(void)
{
    for (uint32_t i = 0; i < AUDIT_RING_SIZE; ++i) {
        uint8_t *p = (uint8_t *)&g_ring[i];
        for (uint32_t k = 0; k < sizeof(g_ring[i]); ++k) p[k] = 0;
    }
    g_write_idx   = 0;
    g_total_count = 0;
    g_current_cnt = 0;
    serial_printf("[AUDIT] init: ring_size=%u entries\n",
                  (unsigned)AUDIT_RING_SIZE);
}

static void copy_path(char *dst, const char *src)
{
    uint32_t i = 0;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (i < AUDIT_PATH_MAX - 1u && src[i]) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

void audit_event(uint32_t type, uint32_t level,
                 uint64_t caller_pid,
                 uint64_t arg0, uint64_t arg1,
                 uint32_t mode, const char *path)
{
    uint64_t flags = irq_save_disable();

    uint64_t idx = g_write_idx & (AUDIT_RING_SIZE - 1u);
    struct audit_entry *e = &g_ring[idx];

    e->seq        = g_write_idx;
    e->tick       = rdtsc();
    e->type       = type;
    e->level      = level;
    e->caller_pid = caller_pid;
    e->arg0       = arg0;
    e->arg1       = arg1;
    e->mode       = mode;
    e->_pad       = 0;
    copy_path(e->path, path);

    g_write_idx++;
    g_total_count++;
    if (g_current_cnt < AUDIT_RING_SIZE) g_current_cnt++;

    irq_restore(flags);

    /* ---- 串口打印，格式与第 8/9 步 audit_* 桩保持一致 ---- */
    const char *ln = level_name(level);
    const char *tn = type_name(type);

    switch (type) {
    case AUDIT_EV_PID_VIOLATION:
        serial_printf("[AUDIT][%s] %s caller_pid=%llu target_pid=%llu\n",
                      ln, tn,
                      (unsigned long long)caller_pid,
                      (unsigned long long)arg0);
        break;

    case AUDIT_EV_KERNEL_MEM_WRITE:
    case AUDIT_EV_KERNEL_MEM_READ:
        serial_printf("[AUDIT][%s] %s caller=%llu addr=0x%llx mode=0x%x\n",
                      ln, tn,
                      (unsigned long long)caller_pid,
                      (unsigned long long)arg0,
                      (unsigned)mode);
        break;

    case AUDIT_EV_CRITICAL_ACCESS:
        serial_printf("[AUDIT][%s] %s caller=%llu path=%s mode=0x%x\n",
                      ln, tn,
                      (unsigned long long)caller_pid,
                      path ? path : "(null)",
                      (unsigned)mode);
        break;

    case AUDIT_EV_SANDBOX_ESCAPE:
        serial_printf("[AUDIT][%s] %s caller=%llu nr=0x%llx reason=%s\n",
                      ln, tn,
                      (unsigned long long)caller_pid,
                      (unsigned long long)arg0,
                      path ? path : "(null)");
        break;

    case AUDIT_EV_ITA_FAILURE:
        serial_printf("[AUDIT][%s] %s path=%s caller=%llu\n",
                      ln, tn,
                      path ? path : "(null)",
                      (unsigned long long)caller_pid);
        break;

    default:
        serial_printf("[AUDIT][%s] %s caller=%llu arg0=0x%llx arg1=0x%llx\n",
                      ln, tn,
                      (unsigned long long)caller_pid,
                      (unsigned long long)arg0,
                      (unsigned long long)arg1);
        break;
    }
}

/* ---------- 高层封装（向后兼容第 8/9 步调用方） ---------- */

void audit_pid_violation(uint64_t caller_pid, uint64_t target_pid)
{
    audit_event(AUDIT_EV_PID_VIOLATION, AUDIT_LVL_CRITICAL,
                caller_pid, target_pid, 0, 0, NULL);
}

void audit_kernel_mem_violation(uint64_t caller_pid, uint64_t vaddr,
                                uint32_t mode)
{
    uint32_t type = (mode & 0x02u) /* OB_ACCESS_WRITE */
                    ? AUDIT_EV_KERNEL_MEM_WRITE
                    : AUDIT_EV_KERNEL_MEM_READ;
    audit_event(type, AUDIT_LVL_CRITICAL,
                caller_pid, vaddr, 0, mode, NULL);
}

void audit_critical_access(uint64_t caller_pid, const char *path,
                           uint32_t mode)
{
    audit_event(AUDIT_EV_CRITICAL_ACCESS, AUDIT_LVL_CRITICAL,
                caller_pid, 0, 0, mode, path);
}

void audit_ita_failure(const char *path, uint64_t caller_pid)
{
    audit_event(AUDIT_EV_ITA_FAILURE, AUDIT_LVL_CRITICAL,
                caller_pid, 0, 0, 0, path);
}

/* ---------- 查询接口 ---------- */

uint64_t audit_total_count(void)
{
    uint64_t flags = irq_save_disable();
    uint64_t v = g_total_count;
    irq_restore(flags);
    return v;
}

uint64_t audit_current_count(void)
{
    uint64_t flags = irq_save_disable();
    uint64_t v = g_current_cnt;
    irq_restore(flags);
    return v;
}

int audit_peek(uint64_t seq, struct audit_entry *out)
{
    if (!out) return -1;

    uint64_t flags = irq_save_disable();

    int found = 0;
    if (g_write_idx > 0) {
        uint64_t start = (g_write_idx > AUDIT_RING_SIZE)
                         ? (g_write_idx - AUDIT_RING_SIZE) : 0;
        uint64_t end   = g_write_idx;
        if (seq >= start && seq < end) {
            uint64_t idx = seq & (AUDIT_RING_SIZE - 1u);
            if (g_ring[idx].seq == seq) {
                *out = g_ring[idx];
                found = 1;
            }
        }
    }

    irq_restore(flags);
    return found ? 0 : -1;
}

void audit_dump_recent(int n)
{
    if (n <= 0) return;

    uint64_t flags = irq_save_disable();
    uint64_t end   = g_write_idx;
    uint64_t start = (end > AUDIT_RING_SIZE) ? (end - AUDIT_RING_SIZE) : 0;
    uint64_t total = end - start;
    if ((uint64_t)n > total) n = (int)total;

    struct audit_entry local[AUDIT_RING_SIZE];  /* 640 x 256 ~ 小 100KB 过大 */
    /* 使用小批量：逐条拷贝 */
    for (int i = 0; i < n; ++i) {
        uint64_t seq = end - (uint64_t)n + (uint64_t)i;
        uint64_t idx = seq & (AUDIT_RING_SIZE - 1u);
        struct audit_entry *e = &g_ring[idx];
        serial_printf("[AUDIT-DUMP] seq=%llu tick=%llu type=%u lvl=%u "
                      "pid=%llu a0=0x%llx a1=0x%llx path=%s\n",
                      (unsigned long long)e->seq,
                      (unsigned long long)e->tick,
                      (unsigned)e->type,
                      (unsigned)e->level,
                      (unsigned long long)e->caller_pid,
                      (unsigned long long)e->arg0,
                      (unsigned long long)e->arg1,
                      e->path[0] ? e->path : "(none)");
    }
    (void)local;

    irq_restore(flags);
}
/*===OmniBridgeOs/kernel/arch/x64/audit.c 结束===*/