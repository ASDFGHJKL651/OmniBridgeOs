#ifndef OMNIBRIDGE_OBINIT_H
#define OMNIBRIDGE_OBINIT_H

#include <stdint.h>

/*
 * /system/init/obinit.toml 的最小解析器（第 12 步）。
 *
 * 支持的 TOML 子集（严格限制，非完整 TOML）：
 *   [service.<name>]
 *   path       = "..."        (必需)
 *   privilege  = 0..9         (可选，默认 5)
 *   pid_hint   = 0..99        (可选，默认 0 = 自动分配)
 *   critical   = true|false   (可选，默认 false)
 *   restart    = true|false   (可选，默认 false)
 *
 * 注释用 #；空行忽略；字符串双引号；数字十进制。
 * 未知键忽略（打印 [OBINIT] WARN）；非法值返回 -1。
 */

struct obinit_service {
    char     name[64];
    char     path[256];
    uint8_t  privilege;
    uint64_t pid_hint;
    int      critical;
    int      restart;
    struct obinit_service *next;
};

struct obinit_config {
    struct obinit_service *head;
    uint32_t count;
};

/* 解析文本。成功返回 0 并填充 out；失败返回 -1，out 不留资源。 */
int  obinit_parse(const char *text, uint64_t len, struct obinit_config *out);

/* 释放 cfg 内部所有节点（不释放 cfg 本身）。 */
void obinit_free(struct obinit_config *cfg);

#endif /* OMNIBRIDGE_OBINIT_H */