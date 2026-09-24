/*===OmniBridgeOs/kernel/arch/x64/oshell.h===*/
#ifndef OMNIBRIDGE_OSHELL_H
#define OMNIBRIDGE_OSHELL_H

#include <stdint.h>
#include <stddef.h>
#include "vfs.h"
#include "task.h"

struct oshell_ctx {
    char cwd[VFS_PATH_MAX];
    struct task_t *task;
    int exit_code;
};

struct oshell_cmd {
    const char *name;
    const char *help;
    int (*fn)(struct oshell_ctx *ctx, int argc, char **argv);
};

extern const struct oshell_cmd g_oshell_cmds[];
extern const uint32_t          g_oshell_cmd_count;

void oshell_init(void);
int oshell_run(void);
int oshell_exec_line(struct oshell_ctx *ctx, const char *line);
struct oshell_ctx *oshell_get_ctx(void);

int oshell_resolve_path(const char *cwd, const char *path,
                        char *out, size_t out_size);

int oshell_tokenize(const char *line,
                    char *buf, size_t buf_size,
                    char *argv[], int max_argc);

/* 第 16 步：sandbox 命令族 */
int cmd_sandbox(struct oshell_ctx *ctx, int argc, char **argv);
int cmd_sandbox_run(struct oshell_ctx *ctx, int argc, char **argv);
int cmd_sandbox_list(struct oshell_ctx *ctx, int argc, char **argv);
int cmd_sandbox_kill(struct oshell_ctx *ctx, int argc, char **argv);
int cmd_sandbox_load_driver(struct oshell_ctx *ctx, int argc, char **argv);

/* 第 17 步：obctl 命令族 */
int cmd_obctl(struct oshell_ctx *ctx, int argc, char **argv);
/* ★ 第 18A 步：网络命令族 */
int cmd_ifconfig(struct oshell_ctx *ctx, int argc, char **argv);
int cmd_route(struct oshell_ctx *ctx, int argc, char **argv);
int cmd_ping(struct oshell_ctx *ctx, int argc, char **argv);
int cmd_dhcp(struct oshell_ctx *ctx, int argc, char **argv);
int cmd_dns(struct oshell_ctx *ctx, int argc, char **argv);
int cmd_netstat(struct oshell_ctx *ctx, int argc, char **argv);
int cmd_curl(struct oshell_ctx *ctx, int argc, char **argv);
/* ★ 第 18B 步：块设备与文件系统命令 */
int cmd_mkfs_obfs(struct oshell_ctx *ctx, int argc, char **argv);
int cmd_fsck_obfs(struct oshell_ctx *ctx, int argc, char **argv);
int cmd_mount(struct oshell_ctx *ctx, int argc, char **argv);
int cmd_umount(struct oshell_ctx *ctx, int argc, char **argv);
int cmd_sync(struct oshell_ctx *ctx, int argc, char **argv);
int cmd_quota(struct oshell_ctx *ctx, int argc, char **argv);
int cmd_blkstat(struct oshell_ctx *ctx, int argc, char **argv);
#endif /* OMNIBRIDGE_OSHELL_H */
/*===OmniBridgeOs/kernel/arch/x64/oshell.h 结束===*/