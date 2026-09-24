#include "obinit.h"
#include "kmalloc.h"
#include "serial.h"

/* ---------- 工具 ---------- */

/* 按长度比较：a[0..alen) 是否等于 b（b 以 '\0' 结尾）。 */
static int str_n_eq(const char *a, uint64_t alen, const char *b)
{
    uint64_t i = 0;
    while (b[i]) {
        if (i >= alen) return 0;
        if (a[i] != b[i]) return 0;
        ++i;
    }
    return i == alen;
}

/* 解析十进制无符号整数（不依赖 NUL 结尾）。 */
static int parse_uint(const char *s, uint64_t len, uint64_t *out)
{
    uint64_t v = 0;
    uint64_t i = 0;
    if (len == 0) return -1;
    while (i < len) {
        char c = s[i];
        if (c < '0' || c > '9') return -1;
        v = v * 10 + (uint64_t)(c - '0');
        if (v > 1000000) return -1;      /* 防溢出 */
        ++i;
    }
    *out = v;
    return 0;
}

/* 从 [s, e) 中取去空白后的 key/value（不复制）。 */
struct kv {
    const char *key;   uint64_t klen;
    const char *val;   uint64_t vlen;
};

static void kv_extract(const char *text, uint64_t s, uint64_t e, struct kv *out)
{
    uint64_t j = s;
    while (j < e && text[j] != '=') ++j;
    uint64_t ks = s, ke = j;
    while (ke > ks && (text[ke - 1] == ' ' || text[ke - 1] == '\t')) --ke;
    uint64_t vs = j + 1, ve = e;
    while (vs < ve && (text[vs] == ' ' || text[vs] == '\t')) ++vs;
    while (ve > vs && (text[ve - 1] == ' ' || text[ve - 1] == '\t')) --ve;
    out->key  = text + ks;
    out->klen = ke - ks;
    out->val  = text + vs;
    out->vlen = ve - vs;
}

/* ---------- 解析主入口 ---------- */

int obinit_parse(const char *text, uint64_t len, struct obinit_config *out)
{
    if (!text || !out) return -1;

    out->head  = 0;
    out->count = 0;

    struct obinit_service *cur = 0;
    int cur_has_path = 0;

    struct obinit_service *head = 0;
    struct obinit_service *tail = 0;
    uint32_t count = 0;

    uint64_t pos = 0;
    while (pos < len) {
        /* 取出一行 [ls, le) */
        uint64_t ls = pos;
        while (pos < len && text[pos] != '\n') ++pos;
        uint64_t le = pos;
        if (pos < len) ++pos;
        if (le > ls && text[le - 1] == '\r') --le;

        /* 去首尾空白 */
        uint64_t s = ls, e = le;
        while (s < e && (text[s] == ' ' || text[s] == '\t')) ++s;
        while (e > s && (text[e - 1] == ' ' || text[e - 1] == '\t')) --e;

        if (s == e) continue;           /* 空行 */
        if (text[s] == '#') continue;   /* 注释 */

        /* ---------- 段 ---------- */
        if (text[s] == '[') {
            uint64_t j = s + 1;
            while (j < e && text[j] != ']') ++j;
            if (j >= e) goto fail;      /* 无 ']' */

            /* 提交上一个服务 */
            if (cur) {
                if (!cur_has_path) { kfree(cur); cur = 0; goto fail; }
                cur->next = 0;
                if (tail) tail->next = cur;
                else      head = cur;
                tail  = cur;
                count++;
                cur = 0;
                cur_has_path = 0;
            }

            const char *sec     = text + s + 1;
            uint64_t    sec_len = j - (s + 1);
            const char *prefix  = "service.";
            const uint64_t plen = 8;

            if (sec_len > plen && str_n_eq(sec, plen, prefix)) {
                uint64_t name_len = sec_len - plen;
                if (name_len == 0 || name_len >= 64) goto fail;

                cur = (struct obinit_service *)kzalloc(sizeof(*cur));
                if (!cur) goto fail;
                for (uint64_t k = 0; k < name_len; ++k) {
                    cur->name[k] = sec[plen + k];
                }
                cur->name[name_len] = '\0';
                cur->path[0]        = '\0';
                cur->privilege      = 5;
                cur->pid_hint       = 0;
                cur->critical       = 0;
                cur->restart        = 0;
                cur_has_path        = 0;
            }
            /* 非 "service." 前缀的段：忽略 */
            continue;
        }

        /* ---------- 键值对 ---------- */
        if (!cur) continue;             /* 段外键值对忽略 */

        struct kv kv;
        kv_extract(text, s, e, &kv);
        /* 检查是否有 '=' */
        {
            uint64_t j = s;
            while (j < e && text[j] != '=') ++j;
            if (j >= e) goto fail;
        }

        if (str_n_eq(kv.key, kv.klen, "path")) {
            if (kv.vlen < 2 ||
                kv.val[0] != '"' || kv.val[kv.vlen - 1] != '"') goto fail;
            uint64_t inner = kv.vlen - 2;
            if (inner >= sizeof(cur->path)) goto fail;
            for (uint64_t k = 0; k < inner; ++k) {
                cur->path[k] = kv.val[1 + k];
            }
            cur->path[inner] = '\0';
            cur_has_path = 1;

        } else if (str_n_eq(kv.key, kv.klen, "privilege")) {
            uint64_t v;
            if (parse_uint(kv.val, kv.vlen, &v) != 0) goto fail;
            if (v > 9) goto fail;
            cur->privilege = (uint8_t)v;

        } else if (str_n_eq(kv.key, kv.klen, "pid_hint")) {
            uint64_t v;
            if (parse_uint(kv.val, kv.vlen, &v) != 0) goto fail;
            if (v > 99) goto fail;
            cur->pid_hint = v;

        } else if (str_n_eq(kv.key, kv.klen, "critical")) {
            if (str_n_eq(kv.val, kv.vlen, "true"))       cur->critical = 1;
            else if (str_n_eq(kv.val, kv.vlen, "false")) cur->critical = 0;
            else goto fail;

        } else if (str_n_eq(kv.key, kv.klen, "restart")) {
            if (str_n_eq(kv.val, kv.vlen, "true"))       cur->restart = 1;
            else if (str_n_eq(kv.val, kv.vlen, "false")) cur->restart = 0;
            else goto fail;

        } else {
            /* 未知键：忽略并打印警告 */
            char kb[32];
            uint64_t kl = kv.klen < 31 ? kv.klen : 31;
            for (uint64_t k = 0; k < kl; ++k) kb[k] = kv.key[k];
            kb[kl] = '\0';
            serial_printf("[OBINIT] WARN: unknown key '%s'\n", kb);
        }
    }

    /* 提交最后一个服务 */
    if (cur) {
        if (!cur_has_path) { kfree(cur); cur = 0; goto fail; }
        cur->next = 0;
        if (tail) tail->next = cur;
        else      head = cur;
        tail = cur;
        count++;
    }

    out->head  = head;
    out->count = count;
    return 0;

fail:
    if (cur) kfree(cur);
    {
        struct obinit_service *p = head;
        while (p) {
            struct obinit_service *nx = p->next;
            kfree(p);
            p = nx;
        }
    }
    return -1;
}

void obinit_free(struct obinit_config *cfg)
{
    if (!cfg) return;
    struct obinit_service *p = cfg->head;
    while (p) {
        struct obinit_service *nx = p->next;
        kfree(p);
        p = nx;
    }
    cfg->head  = 0;
    cfg->count = 0;
}