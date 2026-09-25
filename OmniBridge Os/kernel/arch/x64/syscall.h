/*===OmniBridgeOs/kernel/arch/x64/syscall.h===*/
#ifndef OMNIBRIDGE_SYSCALL_H
#define OMNIBRIDGE_SYSCALL_H

#include <stdint.h>

struct syscall_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
};

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
/* ★ 第 18 步：原生权限检查透传入口 */
#define SYS_OB_CheckAccessNative    0x115u

/* ★ 第 18A 步：网络系统调用 */
#define SYS_OB_Socket       0x120u
#define SYS_OB_Bind         0x121u
#define SYS_OB_Listen       0x122u
#define SYS_OB_Connect      0x123u
#define SYS_OB_Send         0x124u
#define SYS_OB_Recv         0x125u
#define SYS_OB_CloseSocket  0x126u

/* ★ 第 18C 步：用户态运行时系统调用（0x130 起，避开 18A 网络占用）
 *
 * 人工必须审查：
 *   0x120u 段已被网络占用（18A）。0x130u 段专门给用户态运行时。
 *   绝不能重新定义 SYS_OB_UserExit 为 0x120u —— 那会与 SYS_OB_Socket
 *   冲突。此前版本曾用 "先 define 0x120u 再 #undef 再 define 0x130u"
 *   的写法绕过，导致 usr/include/ob/ob.h 与其同时被 include 时
 *   触发 -Wmacro-redefined 警告。本版已清理为单一定义。
 */
#define SYS_OB_UserExit     0x130u
#define SYS_OB_ThreadCreate 0x131u
#define SYS_OB_ThreadJoin   0x132u
#define SYS_OB_SigReturn    0x133u
#define SYS_OB_FutexWait    0x134u
#define SYS_OB_FutexWake    0x135u
#define SYS_OB_TcSetpgrp    0x136u
#define SYS_OB_TcGetpgrp    0x137u
#define SYS_OB_SetFsBase    0x138u
#define SYS_OB_Getpid       0x139u
#define SYS_OB_Getppid      0x13Au
#define SYS_OB_Sleep        0x13Bu
#define SYS_OB_GetTime      0x13Cu
#define SYS_OB_Brk          0x13Du
#define SYS_OB_Sigaction    0x13Eu
#define SYS_OB_Kill         0x13Fu
/* ★ 第 18C 步：pthread 与 TLS 支撑 */
#define SYS_OB_ThreadSpawn  0x140u  /* (entry, arg, ustack_top) -> tid */
#define SYS_OB_ThreadWait   0x141u  /* (tid) -> exit_code */
#define SYS_OB_GetTid       0x143u  /* () -> tid */
/* ★ 第 18D 步：IPC、资源控制与命名空间 */
#define SYS_OB_Pipe         0x150u   /* (int fds[2]) -> 0 */
#define SYS_OB_ShmCreate    0x151u   /* (size) -> shm_id */
#define SYS_OB_ShmMap       0x152u   /* (shm_id, uaddr) -> uaddr */
#define SYS_OB_ShmUnmap     0x153u   /* (uaddr, size) -> 0 */
#define SYS_OB_Poll         0x154u   /* (pollfd*, nfds, timeout_ms) -> nready */
#define SYS_OB_Select       0x155u   /* (nfds, rfd*, wfd*, efd*, tv*) */
#define SYS_OB_EpollCreate  0x156u
#define SYS_OB_EpollCtl     0x157u
#define SYS_OB_EpollWait    0x158u
#define SYS_OB_SetCpuQuota  0x159u   /* (pid, quota) -> 0 */
#define SYS_OB_SetMemQuota  0x15Au   /* (pid, pages) -> 0 */
#define SYS_OB_Seccomp      0x15Bu   /* (mode, filter*) -> 0 */
#define SYS_OB_Ptrace       0x15Cu   /* (request, pid, addr, data) */
#define SYS_OB_Clone        0x15Du   /* (flags, stack) -> pid */
#define SYS_OB_Unshare      0x15Eu   /* (flags) -> 0 */

void syscall_init(void);
int64_t syscall_dispatcher(struct syscall_frame *f);

#endif /* OMNIBRIDGE_SYSCALL_H */
/*===OmniBridgeOs/kernel/arch/x64/syscall.h 结束===*/