/* kernel/arch/x64/compat_path.h
 * 兼容层路径转换（第 18 步）。
 *
 * 关键约束（人工必须审查）：
 *   - 转换后必须通过 compat_path_check()，命中 /kernel/ 或
 *     /system/kernel/ 立即返回 OB_EPERM，命中 /system/critical/ 走
 *     critical_check_access()。
 *   - 不允许出现 "../" 逃逸（转换后路径中不得含 ".." 分量）。
 *   - Windows 盘符不区分大小写。
 */
#ifndef OMNIBRIDGE_COMPAT_PATH_H
#define OMNIBRIDGE_COMPAT_PATH_H

#include <stdint.h>
#include "task.h"

#define COMPAT_PATH_MAX 512

/* Windows 风格路径 -> OB 路径。成功返回 0；负错误码失败。 */
int compat_path_win_to_ob(const char *win_path,
                          char out[COMPAT_PATH_MAX]);

/* Linux 风格路径 -> OB 路径。成功返回 0；负错误码失败。 */
int compat_path_linux_to_ob(const char *linux_path,
                            char out[COMPAT_PATH_MAX]);

/* 统一路径安全检查。
 *   - 命中 /kernel/ 或 /system/kernel/ -> OB_EPERM（并审计）
 *   - 命中 /system/critical/ -> critical_check_access()
 *   - 其他 -> 0
 * cur == NULL 表示内核引导阶段。 */
int compat_path_check(const char *ob_path, struct task_t *cur,
                      uint32_t access_mode);

#endif /* OMNIBRIDGE_COMPAT_PATH_H */