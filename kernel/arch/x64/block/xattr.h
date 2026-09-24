/*===OmniBridgeOs/kernel/arch/x64/block/xattr.h===*/
/*
 * 扩展属性（xattr）基础接口（第 18B 步）。
 *
 * 简化：每个 inode 最多 4 个 xattr 条目，键 ≤ 32 字节，值 ≤ 128 字节。
 * 存储在 inode 磁盘结构的 xattr 区域（预留 512 字节）。
 */
#ifndef OMNIBRIDGE_BLOCK_XATTR_H
#define OMNIBRIDGE_BLOCK_XATTR_H

#include <stdint.h>

#define XATTR_MAX_ENTRIES   4
#define XATTR_KEY_MAX       32
#define XATTR_VALUE_MAX     128

struct xattr_entry {
    char     key[XATTR_KEY_MAX];
    uint8_t  value[XATTR_VALUE_MAX];
    uint16_t value_len;
    uint8_t  used;
    uint8_t  _pad[5];
} __attribute__((packed));

struct xattr_set {
    struct xattr_entry entries[XATTR_MAX_ENTRIES];
    uint32_t count;
    uint32_t _pad;
} __attribute__((packed));

#ifndef OB_ENODATA
#define OB_ENODATA (-61)
#endif
#ifndef OB_ERANGE
#define OB_ERANGE (-34)
#endif

void xattr_init(void);

int xattr_set(struct xattr_set *xs, const char *key,
              const void *value, uint16_t vlen);
int xattr_get(const struct xattr_set *xs, const char *key,
              void *out, uint16_t *out_len);
int xattr_remove(struct xattr_set *xs, const char *key);
int xattr_list(const struct xattr_set *xs,
               char keys[][XATTR_KEY_MAX], uint32_t max, uint32_t *out_n);

#endif /* OMNIBRIDGE_BLOCK_XATTR_H */
/*===OmniBridgeOs/kernel/arch/x64/block/xattr.h 结束===*/