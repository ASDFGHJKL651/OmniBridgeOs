/*===OmniBridgeOs/kernel/arch/x64/user/account.h===*/
#ifndef OMNIBRIDGE_USER_ACCOUNT_H
#define OMNIBRIDGE_USER_ACCOUNT_H

#include <stdint.h>

#define ACCT_NAME_MAX   32
#define ACCT_HASH_LEN   32     /* SHA-256 */
#define ACCT_MAX_USERS  16
#define ACCT_MAX_GROUPS 16

struct acct_user {
    char     name[ACCT_NAME_MAX];
    uint32_t uid;
    uint32_t gid;
    char     home[128];
    char     shell[128];
    uint8_t  passwd_hash[ACCT_HASH_LEN];
    uint8_t  used;
};

struct acct_group {
    char     name[ACCT_NAME_MAX];
    uint32_t gid;
    uint32_t members[8];
    uint8_t  member_count;
    uint8_t  used;
};

struct acct_session {
    uint32_t uid;
    uint32_t gid;
    uint64_t sid;             /* session id */
    uint64_t started_tick;
    uint64_t tty_pgid;
    uint8_t  valid;
    uint8_t  _pad[7];
};

void account_init(void);

/* 用户数据库 */
int account_add_user(const char *name, uint32_t uid, uint32_t gid,
                     const char *home, const char *shell,
                     const char *password);
int account_lookup_uid(uint32_t uid, struct acct_user *out);
int account_lookup_name(const char *name, struct acct_user *out);

/* 密码 */
int account_set_password(const char *name, const char *new_pass);
int account_check_password(const char *name, const char *pass);

/* 会话 */
int account_login(const char *name, const char *pass,
                  struct acct_session *out);
int account_logout(struct acct_session *s);
int account_whoami(char *out, uint32_t out_size);
uint32_t account_current_uid(void);

/* 会话密钥环（简化：每条 32 字节） */
#define KEYRING_MAX 8
struct acct_keyring {
    uint8_t  keys[KEYRING_MAX][32];
    uint8_t  used[KEYRING_MAX];
    uint8_t  count;
    uint8_t  _pad[7];
};

int account_keyring_put(const uint8_t key[32]);
int account_keyring_find(const uint8_t key[32]);

#endif /* OMNIBRIDGE_USER_ACCOUNT_H */
/*===OmniBridgeOs/kernel/arch/x64/user/account.h 结束===*/