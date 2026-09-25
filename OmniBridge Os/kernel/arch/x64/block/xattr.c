/*===OmniBridgeOs/kernel/arch/x64/block/xattr.c===*/
#include "xattr.h"
#include "serial.h"

void xattr_init(void)
{
    serial_printf("[XATTR] init: entries=%u key_max=%u value_max=%u\n",
                  (unsigned)XATTR_MAX_ENTRIES,
                  (unsigned)XATTR_KEY_MAX,
                  (unsigned)XATTR_VALUE_MAX);
}

static int key_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

static int key_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
    return i;
}

int xattr_set(struct xattr_set *xs, const char *key,
              const void *value, uint16_t vlen)
{
    if (!xs || !key || !value) return -22;
    if (vlen > XATTR_VALUE_MAX) return -34;
    int klen = 0;
    while (key[klen]) ++klen;
    if (klen == 0 || klen >= XATTR_KEY_MAX) return -22;

    /* 更新已有 */
    for (uint32_t i = 0; i < XATTR_MAX_ENTRIES; ++i) {
        if (xs->entries[i].used && key_eq(xs->entries[i].key, key)) {
            const uint8_t *v = (const uint8_t *)value;
            for (uint16_t k = 0; k < vlen; ++k)
                xs->entries[i].value[k] = v[k];
            xs->entries[i].value_len = vlen;
            return 0;
        }
    }
    /* 新增 */
    for (uint32_t i = 0; i < XATTR_MAX_ENTRIES; ++i) {
        if (!xs->entries[i].used) {
            struct xattr_entry *e = &xs->entries[i];
            key_copy(e->key, key, XATTR_KEY_MAX);
            const uint8_t *v = (const uint8_t *)value;
            for (uint16_t k = 0; k < vlen; ++k) e->value[k] = v[k];
            e->value_len = vlen;
            e->used = 1;
            xs->count++;
            return 0;
        }
    }
    return -12;  /* -ENOMEM */
}

int xattr_get(const struct xattr_set *xs, const char *key,
              void *out, uint16_t *out_len)
{
    if (!xs || !key || !out || !out_len) return -22;
    for (uint32_t i = 0; i < XATTR_MAX_ENTRIES; ++i) {
        if (xs->entries[i].used && key_eq(xs->entries[i].key, key)) {
            if (*out_len < xs->entries[i].value_len) {
                *out_len = xs->entries[i].value_len;
                return -34;
            }
            uint8_t *d = (uint8_t *)out;
            for (uint16_t k = 0; k < xs->entries[i].value_len; ++k)
                d[k] = xs->entries[i].value[k];
            *out_len = xs->entries[i].value_len;
            return 0;
        }
    }
    return -61;  /* -ENODATA */
}

int xattr_remove(struct xattr_set *xs, const char *key)
{
    if (!xs || !key) return -22;
    for (uint32_t i = 0; i < XATTR_MAX_ENTRIES; ++i) {
        if (xs->entries[i].used && key_eq(xs->entries[i].key, key)) {
            xs->entries[i].used = 0;
            xs->entries[i].value_len = 0;
            xs->entries[i].key[0] = '\0';
            if (xs->count > 0) xs->count--;
            return 0;
        }
    }
    return -61;
}

int xattr_list(const struct xattr_set *xs,
               char keys[][XATTR_KEY_MAX], uint32_t max, uint32_t *out_n)
{
    if (!xs || !keys || !out_n) return -22;
    uint32_t n = 0;
    for (uint32_t i = 0; i < XATTR_MAX_ENTRIES && n < max; ++i) {
        if (xs->entries[i].used) {
            key_copy(keys[n], xs->entries[i].key, XATTR_KEY_MAX);
            n++;
        }
    }
    *out_n = n;
    return 0;
}
/*===OmniBridgeOs/kernel/arch/x64/block/xattr.c 结束===*/