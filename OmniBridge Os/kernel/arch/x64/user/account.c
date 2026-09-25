/*===OmniBridgeOs/kernel/arch/x64/user/account.c===*/
#include "account.h"
#include "sha256.h"
#include "serial.h"
#include "audit.h"
#include "sched.h"
#include "task.h"

static struct acct_user    g_users[ACCT_MAX_USERS];
static struct acct_group   g_groups[ACCT_MAX_GROUPS];
static uint32_t            g_user_count = 0;
static uint32_t            g_group_count = 0;

static struct acct_session g_session;
static struct acct_keyring g_keyring;

static uint64_t strnlen_(const char *s, uint64_t max)
{
    uint64_t n = 0;
    if (!s) return 0;
    while (n < max && s[n]) ++n;
    return n;
}

static void str_copy(char *dst, const char *src, uint64_t cap)
{
    uint64_t i = 0;
    if (!dst || cap == 0) return;
    if (src) while (src[i] && i + 1 < cap) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}

static int str_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; ++a; ++b; }
    return *a == '\0' && *b == '\0';
}

/*
 * 人工必须审查：
 *   - 本步使用 SHA-256(name || ':' || pass)，无盐。
 *   - 生产环境必须使用带盐的 Argon2/bcrypt/scrypt。
 *   - 不得以明文形式存储或打印密码。
 */
static void hash_password(const char *name, const char *pass, uint8_t out[32])
{
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    if (name) sha256_update(&ctx, name, strnlen_(name, ACCT_NAME_MAX));
    sha256_update(&ctx, ":", 1);
    if (pass) sha256_update(&ctx, pass, strnlen_(pass, 128));
    sha256_final(&ctx, out);
}

void account_init(void)
{
    for (int i = 0; i < ACCT_MAX_USERS; ++i) g_users[i].used = 0;
    for (int i = 0; i < ACCT_MAX_GROUPS; ++i) g_groups[i].used = 0;
    g_user_count = 0;
    g_group_count = 0;
    g_session.valid = 0;

    uint8_t *p = (uint8_t *)&g_keyring;
    for (unsigned i = 0; i < sizeof(g_keyring); ++i) p[i] = 0;

    /* ★ 18C：创建内置组 */
    static const struct { const char *name; uint32_t gid; } groups[] = {
        { "root",  0 },
        { "wheel", 10 },
        { "users", 1000 },
    };
    for (unsigned i = 0; i < sizeof(groups)/sizeof(groups[0]); ++i) {
        struct acct_group *g = &g_groups[g_group_count++];
        str_copy(g->name, groups[i].name, ACCT_NAME_MAX);
        g->gid = groups[i].gid;
        g->member_count = 0;
        g->used = 1;
    }

    account_add_user("root", 0, 0, "/root", "/bin/osh", "root");
    account_add_user("user", 1000, 1000, "/home/user", "/bin/osh", "user");

    serial_printf("[ACCT] init: users=%u groups=%u (root/user seeded)\n",
                  (unsigned)g_user_count, (unsigned)g_group_count);
}

int account_add_user(const char *name, uint32_t uid, uint32_t gid,
                     const char *home, const char *shell,
                     const char *password)
{
    if (!name || g_user_count >= ACCT_MAX_USERS) return -1;
    struct acct_user *u = &g_users[g_user_count++];
    str_copy(u->name, name, ACCT_NAME_MAX);
    u->uid = uid;
    u->gid = gid;
    str_copy(u->home, home ? home : "/", 128);
    str_copy(u->shell, shell ? shell : "/bin/osh", 128);
    hash_password(name, password ? password : "", u->passwd_hash);
    u->used = 1;
    return 0;
}

int account_lookup_uid(uint32_t uid, struct acct_user *out)
{
    for (uint32_t i = 0; i < g_user_count; ++i) {
        if (g_users[i].used && g_users[i].uid == uid) {
            if (out) *out = g_users[i];
            return 0;
        }
    }
    return -1;
}

int account_lookup_name(const char *name, struct acct_user *out)
{
    for (uint32_t i = 0; i < g_user_count; ++i) {
        if (g_users[i].used && str_eq(g_users[i].name, name)) {
            if (out) *out = g_users[i];
            return 0;
        }
    }
    return -1;
}

int account_check_password(const char *name, const char *pass)
{
    struct acct_user u;
    if (account_lookup_name(name, &u) != 0) return -1;
    uint8_t h[32];
    hash_password(name, pass ? pass : "", h);
    uint8_t diff = 0;
    for (int i = 0; i < 32; ++i) diff |= (h[i] ^ u.passwd_hash[i]);
    return diff == 0 ? 0 : -1;
}

int account_set_password(const char *name, const char *new_pass)
{
    for (uint32_t i = 0; i < g_user_count; ++i) {
        if (g_users[i].used && str_eq(g_users[i].name, name)) {
            hash_password(name, new_pass ? new_pass : "",
                          g_users[i].passwd_hash);
            return 0;
        }
    }
    return -1;
}

int account_login(const char *name, const char *pass,
                  struct acct_session *out)
{
    struct acct_user u;
    if (account_lookup_name(name, &u) != 0) {
        audit_event(AUDIT_EV_CRITICAL_ACCESS, AUDIT_LVL_WARN,
                    0, 0, 0, 0, "(login-fail-unknown)");
        return -1;
    }
    if (account_check_password(name, pass) != 0) {
        audit_event(AUDIT_EV_CRITICAL_ACCESS, AUDIT_LVL_CRITICAL,
                    0, u.uid, 0, 0, "(login-fail-bad-pass)");
        return -1;
    }

    g_session.uid = u.uid;
    g_session.gid = u.gid;
    g_session.sid = 1;
    g_session.started_tick = 0;
    g_session.valid = 1;

    serial_printf("[ACCT] login OK user=%s uid=%u gid=%u\n",
                  u.name, (unsigned)u.uid, (unsigned)u.gid);
    audit_event(AUDIT_EV_CRITICAL_ACCESS, AUDIT_LVL_INFO,
                0, u.uid, 0, 0, "(login-ok)");

    if (out) *out = g_session;
    return 0;
}

int account_logout(struct acct_session *s)
{
    (void)s;
    g_session.valid = 0;
    return 0;
}

int account_whoami(char *out, uint32_t out_size)
{
    if (!out || out_size == 0) return -1;
    if (!g_session.valid) {
        str_copy(out, "nobody", out_size);
        return 0;
    }
    struct acct_user u;
    if (account_lookup_uid(g_session.uid, &u) != 0) {
        str_copy(out, "unknown", out_size);
        return -1;
    }
    str_copy(out, u.name, out_size);
    return 0;
}

uint32_t account_current_uid(void)
{
    return g_session.valid ? g_session.uid : 0xFFFFFFFFu;
}

int account_keyring_put(const uint8_t key[32])
{
    if (!key) return -1;
    for (int i = 0; i < KEYRING_MAX; ++i) {
        if (!g_keyring.used[i]) {
            for (int k = 0; k < 32; ++k) g_keyring.keys[i][k] = key[k];
            g_keyring.used[i] = 1;
            if (g_keyring.count < KEYRING_MAX) g_keyring.count++;
            return 0;
        }
    }
    return -1;
}

int account_keyring_find(const uint8_t key[32])
{
    if (!key) return 0;
    for (int i = 0; i < KEYRING_MAX; ++i) {
        if (!g_keyring.used[i]) continue;
        uint8_t diff = 0;
        for (int k = 0; k < 32; ++k) diff |= (g_keyring.keys[i][k] ^ key[k]);
        if (diff == 0) return 1;
    }
    return 0;
}
/*===OmniBridgeOs/kernel/arch/x64/user/account.c 结束===*/