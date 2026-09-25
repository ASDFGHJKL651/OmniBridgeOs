#ifndef OMNIBRIDGE_CRITICAL_H
#define OMNIBRIDGE_CRITICAL_H

#include <stdint.h>
#include "task.h"

/*
 * /system/critical/ 挂载路径与 ACL 判定接口（第 12 步）。
 *
 * ACL 语义（与 v5.0 §10.5.2 一致）：
 *   1) 内核引导阶段（无 task 上下文，cur == NULL）：只读允许，写/删拒绝。
 *   2) 沙盒进程（sandbox_flags != 0）：无条件拒绝（最高优先级）。
 *   3) PID 1..99 系统保留区（非沙盒）：允许任何操作。
 *   4) 权限 9 且 ui_token_valid：允许任何操作。
 *   5) 其他所有用户态进程：拒绝（并写审计）。
 *
 * 检查顺序不可调整；任何调整都可能引入绕过窗口。
 */

#define CRITICAL_PATH       "/system/critical"
#define CRITICAL_PATH_LEN   16
#define CRITICAL_SUBPREFIX  "/system/critical/"

/* 保留常量；当前 critical_check_access() 直接使用 OB_ACCESS_* */
#define CRITICAL_MODE_RW  0x01
#define CRITICAL_MODE_RO  0x02

/* 初始化：在根 VFS 上创建 /system 目录，并把独立的 tmpfs 挂载到
 * /system/critical。幂等：重复调用直接返回。 */
void critical_init(void);

/* 子系统的"只读"语义查询（本步恒为 1）。 */
int  critical_is_read_only(void);

/* path 恰好是 /system/critical 时返回 1。 */
int path_is_critical_exact(const char *path);

/* path 是 /system/critical 或其后代时返回 1。
 * 判定规则与 permission.c 的 path_is_critical_dir 一致。 */
int path_is_critical_path(const char *path);

/* ACL 决策：
 *   返回 0 允许；负错误码拒绝（通常 OB_EPERM）。
 *   cur == NULL 表示内核引导阶段（无 task 上下文）。
 *   path 不命中 /system/critical 前缀时返回 0（不拦截）。 */
int critical_check_access(struct task_t *cur, const char *path,
                          uint32_t access_mode);

#endif /* OMNIBRIDGE_CRITICAL_H */