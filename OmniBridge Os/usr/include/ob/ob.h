/*===OmniBridgeOs/usr/include/ob/ob.h===*/
#ifndef OB_USER_OB_H
#define OB_USER_OB_H

#include <stdint.h>
#include <stddef.h>

/* 系统调用号（与 kernel/arch/x64/syscall.h 一致） */
#define SYS_OB_CreateProcess        0x100u
#define SYS_OB_TerminateProcess     0x101u
#define SYS_OB_GetProcessInfo       0x102u
#define SYS_OB_SendSignal           0x103u
#define SYS_OB_GetCurrentToken      0x104u
#define SYS_OB_CheckAccess          0x105u
#define SYS_OB_OpenFile             0x106u
#define SYS_OB_ReadFile             0x107u
#define SYS_OB_WriteFile            0x108u
#define SYS_OB_CloseHandle          0x109u
#define SYS_OB_VirtualAlloc         0x10Au
#define SYS_OB_VirtualFree          0x10Bu
#define SYS_OB_LoadDriver           0x10Cu
#define SYS_OB_LoadDriverSandboxed  0x10Du
#define SYS_OB_RegisterInterrupt    0x10Eu
#define SYS_OB_CreateSandboxProcess 0x10Fu
#define SYS_OB_Compat_Preload       0x110u
#define SYS_OB_ReadKernelMemory     0x111u
#define SYS_OB_WriteKernelMemory    0x112u
#define SYS_OB_ReadKernelFile       0x113u
#define SYS_OB_InternalSign         0x114u
#define SYS_OB_CheckAccessNative    0x115u
/* ★ 第 18A 步：网络 */
#define SYS_OB_Socket               0x120u
#define SYS_OB_Bind                 0x121u
#define SYS_OB_Listen               0x122u
#define SYS_OB_Connect              0x123u
#define SYS_OB_Send                 0x124u
#define SYS_OB_Recv                 0x125u
#define SYS_OB_CloseSocket          0x126u
/* ★ 第 18C 步：用户态运行时（0x130 起，避开 18A 网络占用） */
#define SYS_OB_UserExit             0x130u
#define SYS_OB_ThreadCreate         0x131u
#define SYS_OB_ThreadJoin           0x132u
#define SYS_OB_SigReturn            0x133u
#define SYS_OB_FutexWait            0x134u
#define SYS_OB_FutexWake            0x135u
#define SYS_OB_TcSetpgrp            0x136u
#define SYS_OB_TcGetpgrp            0x137u
#define SYS_OB_SetFsBase            0x138u
#define SYS_OB_Getpid               0x139u
#define SYS_OB_Getppid              0x13Au
#define SYS_OB_Sleep                0x13Bu
#define SYS_OB_GetTime              0x13Cu
#define SYS_OB_Brk                  0x13Du
#define SYS_OB_Sigaction            0x13Eu
#define SYS_OB_Kill                 0x13Fu
#define SYS_OB_ThreadSpawn  0x140u  /* (entry, arg, ustack_top) -> tid */
#define SYS_OB_ThreadWait   0x141u  /* (tid) -> exit_code */
#define SYS_OB_GetTid       0x143u  /* () -> tid */
/* ★ 第 18D 步：IPC、资源控制与命名空间 */
#define SYS_OB_Pipe         0x150u
#define SYS_OB_ShmCreate    0x151u
#define SYS_OB_ShmMap       0x152u
#define SYS_OB_ShmUnmap     0x153u
#define SYS_OB_Poll         0x154u
#define SYS_OB_Select       0x155u
#define SYS_OB_EpollCreate  0x156u
#define SYS_OB_EpollCtl     0x157u
#define SYS_OB_EpollWait    0x158u
#define SYS_OB_SetCpuQuota  0x159u
#define SYS_OB_SetMemQuota  0x15Au
#define SYS_OB_Seccomp      0x15Bu
#define SYS_OB_Ptrace       0x15Cu
#define SYS_OB_Clone        0x15Du
#define SYS_OB_Unshare      0x15Eu

/* ============================================================
 * ★ 本轮新增：权限检查常量（与 kernel/arch/x64/permission.h 一致）
 *
 * 人工必须审查：
 *   这些常量必须与内核侧 permission.h 严格一致。任何一端修改都要
 *   同步更新另一端，否则 OB_CheckAccessNative 的语义会静默错位。
 * ============================================================ */

/* 资源类型 */
#define OB_RES_FILE     0
#define OB_RES_MEMORY   1
#define OB_RES_PROCESS  2
#define OB_RES_IPC      3
#define OB_RES_CONFIG   4

/* 访问模式 */
#define OB_ACCESS_READ   0x01
#define OB_ACCESS_WRITE  0x02
#define OB_ACCESS_EXEC   0x04
#define OB_ACCESS_DELETE 0x08
#define OB_ACCESS_ALL    0x0F

/* OB_CheckAccess / OB_CheckAccessNative 返回值
 *
 * 人工必须审查：
 *   内核对 ALLOWED 返回 1，对 DENIED 返回 0。
 *   这与常见的 "0 表示成功" 惯例相反，是本内核的既定约定。
 */
#define OB_PERM_DENIED   0
#define OB_PERM_ALLOWED  1

/* 内联 syscall 封装：MS ABI 前 4 个整数参数在 rcx/rdx/r8/r9 */
static inline int64_t ob_syscall4(uint64_t nr, uint64_t a1, uint64_t a2,
                                  uint64_t a3, uint64_t a4)
{
    int64_t ret;
    register uint64_t r10 __asm__("r10") = a4;
    __asm__ __volatile__(
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
        : "rcx", "r11", "cc", "memory");   /* ★ 新增 "cc" */
    return ret;
}

static inline int64_t ob_syscall0(uint64_t nr)
{ return ob_syscall4(nr, 0, 0, 0, 0); }
static inline int64_t ob_syscall1(uint64_t nr, uint64_t a1)
{ return ob_syscall4(nr, a1, 0, 0, 0); }
static inline int64_t ob_syscall2(uint64_t nr, uint64_t a1, uint64_t a2)
{ return ob_syscall4(nr, a1, a2, 0, 0); }
static inline int64_t ob_syscall3(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3)
{ return ob_syscall4(nr, a1, a2, a3, 0); }

struct obr_header_user {
    uint32_t magic;
    uint16_t version;
    uint8_t  arch;
    uint8_t  min_privilege;
    uint64_t entry_point;
    uint64_t ph_offset;
    uint16_t ph_count;
    uint16_t _pad0;
    uint64_t sh_offset;
    uint16_t sh_count;
    uint16_t _pad1;
    uint64_t dep_count;
    uint64_t dep_strings_offset;
    uint32_t checksum;
    uint8_t  sig_type;
    uint8_t  sig_padding[3];
    uint32_t sig_length;
} __attribute__((packed));

#endif /* OB_USER_OB_H */
/*===OmniBridgeOs/usr/include/ob/ob.h 结束===*/