#include "syscall.h"
#include "printk.h"
#include "serial.h"

extern void syscall_entry(void);

/* ---------- MSR ---------- */
#define MSR_EFER   0xC0000080u
#define MSR_STAR   0xC0000081u
#define MSR_LSTAR  0xC0000082u
#define MSR_FMASK  0xC0000084u

#define EFER_SCE   (1ULL << 0)

static inline void wrmsr(uint32_t msr, uint64_t val)
{
    uint32_t lo = (uint32_t)(val & 0xFFFFFFFF);
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ __volatile__("wrmsr"
                         :: "c"(msr), "a"(lo), "d"(hi)
                         : "memory");
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr"
                         : "=a"(lo), "=d"(hi)
                         : "c"(msr)
                         : "memory");
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

void syscall_init(void)
{
    /* 1) EFER.SCE = 1 */
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_EFER, efer);

    /*
     * 2) STAR：
     *   [47:32] = SYSCALL 后的内核 CS（SS = +8）
     *   [63:48] = SYSRET 的基准；SYSRET 时 CS = 基准+16、SS = 基准+8
     *
     * 当前 GDT 中 user code = 0x18（索引 3），user data = 0x20（索引 4）。
     * 使用 0x1B 作为基准，SYSRET 后 CS = 0x2B（索引 5 = TSS 描述符低半），
     * SS = 0x23（索引 4 = user data）。取指时以 CPL=3 访问内核高半区
     * （U/S=0）会触发 #PF，这是本步骤"尚无用户态"的已知限制，
     * 正式用户态支持阶段会调整 GDT 布局或改用 iretq 返回。
     */
    uint64_t star = ((uint64_t)0x1Bu << 48) | ((uint64_t)0x08u << 32);
    wrmsr(MSR_STAR, star);

    /* 3) LSTAR = syscall_entry 的虚拟地址 */
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);

    /* 4) FMASK：进入时清除 IF、TF、DF、NT、AC */
    wrmsr(MSR_FMASK, 0x47700ULL);

    serial_printf("[SYSCALL] EFER.SCE=1 STAR=0x%llx LSTAR=0x%llx FMASK=0x47700\n",
                  (unsigned long long)star,
                  (unsigned long long)(uintptr_t)syscall_entry);
}

int64_t syscall_dispatcher(struct syscall_frame *f)
{
    /*
     * 骨架：仅打印系统调用号与参数，返回 -ENOSYS。
     * 参数约定（Linux 风格）：
     *   rax = 系统调用号
     *   rdi, rsi, rdx, r10, r8, r9 = 参数 1..6
     */
    serial_printf("[SYSCALL] nr=%llu arg0=0x%llx arg1=0x%llx "
                  "arg2=0x%llx arg3=0x%llx\n",
                  (unsigned long long)f->rax,
                  (unsigned long long)f->rdi,
                  (unsigned long long)f->rsi,
                  (unsigned long long)f->rdx,
                  (unsigned long long)f->r10);

    return -38;  /* -ENOSYS */
}