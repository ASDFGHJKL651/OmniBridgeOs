/*===OmniBridgeOs/kernel/arch/x64/ed25519.c===*/
/*
 * RFC 8032 Ed25519 —— TweetNaCl 移植版。
 *
 * 与上游 TweetNaCl 的差异：
 *   - crypto_hash 替换为 sha512.h；
 *   - 移除 randombytes（密钥从外部 seed 派生）；
 *   - 提供最小化常量时间比较 ct_verify_32；
 *   - API 拆分：ed25519_keypair_from_seed / sign / verify，
 *     签名输出为独立的 64 字节 sig 缓冲，不再与消息拼接。
 */
#include "ed25519.h"
#include "sha512.h"

/* ---------- 类型别名 ---------- */
typedef int64_t  i64;
typedef uint64_t u64;
typedef uint8_t  u8;

/* 群元素：16 个 16-bit limb，用 int64_t 存储以容纳中间值 */
typedef i64 gf[16];

/* ---------- 常量 ---------- */

static const gf gf0 = {0, 0, 0, 0, 0, 0, 0, 0,
                       0, 0, 0, 0, 0, 0, 0, 0};
static const gf gf1 = {1, 0, 0, 0, 0, 0, 0, 0,
                       0, 0, 0, 0, 0, 0, 0, 0};

/* d = -121665/121666 mod p */
static const gf D = {
    0x78a3, 0x1359, 0x4dca, 0x75eb,
    0xd8ab, 0x4141, 0x0a4d, 0x0070,
    0xe898, 0x7779, 0x4079, 0x8cc7,
    0xfe73, 0x2b6f, 0x6cee, 0x5203,
};

/* 2*d */
static const gf D2 = {
    0xf159, 0x26b2, 0x9b94, 0xebd6,
    0xb156, 0x8283, 0x149a, 0x00e0,
    0xd130, 0xeef3, 0x80f2, 0x198e,
    0xfce7, 0x56df, 0xd9dc, 0x2406,
};

/* 基点 B 的 x 坐标 */
static const gf X = {
    0xd51a, 0x8f25, 0x2d60, 0xc956,
    0xa7b2, 0x9525, 0xc760, 0x692c,
    0xdc5c, 0xfdd6, 0xe231, 0xc0a4,
    0x53fe, 0xcd6e, 0x36d3, 0x2169,
};

/* 基点 B 的 y 坐标：y = 4/5 mod p */
static const gf Y = {
    0x6658, 0x6666, 0x6666, 0x6666,
    0x6666, 0x6666, 0x6666, 0x6666,
    0x6666, 0x6666, 0x6666, 0x6666,
    0x6666, 0x6666, 0x6666, 0x6666,
};

/* sqrt(-1) = 2^((p-1)/4) mod p */
static const gf I = {
    0xa0b0, 0x4a0e, 0x1b27, 0xc4ee,
    0xe478, 0xad2f, 0x1806, 0x2f43,
    0xd7a7, 0x3dfb, 0x0099, 0x2b4d,
    0xdf0b, 0x4fc1, 0x2480, 0x2b83,
};

/* 群阶 L（32 个 8-bit limb，little-endian） */
static const uint32_t L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0x10,
};

/* ---------- 工具 ---------- */

static void mem_zero(void *p, uint64_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    for (uint64_t i = 0; i < n; ++i) b[i] = 0;
}

/* 常量时间相等：x == y 返回 1，否则返回 0 */
static int ct_verify_32(const uint8_t *x, const uint8_t *y)
{
    uint8_t d = 0;
    for (int i = 0; i < 32; ++i) {
        d |= (uint8_t)(x[i] ^ y[i]);
    }
    /* d == 0 → 1；d != 0 → 0 */
    return (int)(((uint64_t)d - 1) >> 63);
}

/* ---------- 有限域（mod 2^255 - 19） ---------- */

static void set25519(gf r, const gf a)
{
    for (int i = 0; i < 16; ++i) r[i] = a[i];
}

/* Carry 归一化，使每 limb 落在 [0, 2^16) */
static void car25519(gf o)
{
    for (int i = 0; i < 16; ++i) {
        o[i] += (1LL << 16);
        i64 c = o[i] >> 16;
        o[(i + 1) * (i < 15 ? 1 : 0)] += c - 1
                                       + 37 * (c - 1) * (i == 15 ? 1 : 0);
        o[i] -= c << 16;
    }
}

/* 常量时间条件交换：b == 1 时交换 p 和 q */
static void sel25519(gf p, gf q, int b)
{
    i64 c = ~(b - 1);
    for (int i = 0; i < 16; ++i) {
        i64 t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

/* 编码为 32 字节 little-endian */
static void pack25519(u8 *o, const gf n)
{
    gf m, t;
    for (int i = 0; i < 16; ++i) t[i] = n[i];
    car25519(t);
    car25519(t);
    car25519(t);

    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }

    for (int i = 0; i < 16; ++i) {
        o[2 * i]     = (u8)(t[i] & 0xff);
        o[2 * i + 1] = (u8)((t[i] >> 8) & 0xff);
    }
}

/* 返回 1 表示 a != b，0 表示 a == b */
static int neq25519(const gf a, const gf b)
{
    u8 c[32], d[32];
    pack25519(c, a);
    pack25519(d, b);
    return !ct_verify_32(c, d);
}

static u8 par25519(const gf a)
{
    u8 d[32];
    pack25519(d, a);
    return d[0] & 1;
}

/* 解码 32 字节为有限域元素（清除最高位） */
static void unpack25519(gf o, const u8 *n)
{
    for (int i = 0; i < 16; ++i) {
        o[i] = (i64)n[2 * i] + ((i64)n[2 * i + 1] << 8);
    }
    o[15] &= 0x7fff;
}

/* 加法：o = a + b */
static void A(gf o, const gf a, const gf b)
{
    for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i];
}

/* 减法：o = a - b */
static void Z(gf o, const gf a, const gf b)
{
    for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i];
}

/* 乘法：o = a * b mod p */
static void M(gf o, const gf a, const gf b)
{
    i64 t[31];
    for (int i = 0; i < 31; ++i) t[i] = 0;
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 16; ++j) {
            t[i + j] += a[i] * b[j];
        }
    }
    /* 归约 2^256 = 38 mod p 的高位 */
    for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    car25519(o);
    car25519(o);
}

/* 平方：o = a^2 */
static void S(gf o, const gf a)
{
    M(o, a, a);
}

/* 求逆：o = 1/a = a^(p-2) */
static void inv25519(gf o, const gf i)
{
    gf c;
    for (int a = 0; a < 16; ++a) c[a] = i[a];
    for (int a = 253; a >= 0; --a) {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, i);
    }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}

/* o = i^((p-5)/8) = i^(2^252 - 3) */
static void pow2523(gf o, const gf i)
{
    gf c;
    for (int a = 0; a < 16; ++a) c[a] = i[a];
    for (int a = 250; a >= 0; --a) {
        S(c, c);
        if (a != 1) M(c, c, i);
    }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}

/* ---------- 群运算（扩展坐标） ---------- */

/* p = p + q */
static void add(gf p[4], gf q[4])
{
    gf a, b, c, d, t, e, f, g, h;

    Z(a, p[1], p[0]);
    Z(t, q[1], q[0]);
    M(a, a, t);
    A(b, p[0], p[1]);
    A(t, q[0], q[1]);
    M(b, b, t);
    M(c, p[3], q[3]);
    M(c, c, D2);
    M(d, p[2], q[2]);
    A(d, d, d);
    Z(e, b, a);
    Z(f, d, c);
    A(g, d, c);
    A(h, b, a);

    M(p[0], e, f);
    M(p[1], h, g);
    M(p[2], g, f);
    M(p[3], e, h);
}

/* 常量时间条件交换两组点 */
static void cswap(gf p[4], gf q[4], u8 b)
{
    for (int i = 0; i < 4; ++i) {
        sel25519(p[i], q[i], b);
    }
}

/* 编码点：32 字节，最高位 = x 的最低位 */
static void pack(u8 *r, gf p[4])
{
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    M(tx, p[0], zi);
    M(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= (u8)(par25519(tx) << 7);
}

/* 标量乘：p = [s]q */
static void scalarmult(gf p[4], gf q[4], const u8 *s)
{
    set25519(p[0], gf0);
    set25519(p[1], gf1);
    set25519(p[2], gf1);
    set25519(p[3], gf0);
    for (int i = 255; i >= 0; --i) {
        u8 b = (u8)((s[i / 8] >> (i & 7)) & 1);
        cswap(p, q, b);
        add(q, p);
        add(p, p);
        cswap(p, q, b);
    }
}

/* 基点标量乘：p = [s]B */
static void scalarbase(gf p[4], const u8 *s)
{
    gf q[4];
    set25519(q[0], X);
    set25519(q[1], Y);
    set25519(q[2], gf1);
    M(q[3], X, Y);
    scalarmult(p, q, s);
}

/* ---------- 标量 mod L ---------- */

static void modL(u8 *r, i64 x[64])
{
    i64 carry, i, j;

    for (i = 63; i >= 32; --i) {
        carry = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * (i64)L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry << 8;
        }
        x[j] += carry;
        x[i] = 0;
    }

    carry = 0;
    for (j = 0; j < 32; ++j) {
        x[j] += carry - (x[31] >> 4) * (i64)L[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (j = 0; j < 32; ++j) x[j] -= carry * (i64)L[j];
    for (i = 0; i < 32; ++i) {
        x[i + 1] += x[i] >> 8;
        r[i] = (u8)(x[i] & 255);
    }
}

static void reduce(u8 *r)
{
    i64 x[64];
    for (int i = 0; i < 64; ++i) x[i] = (u64)r[i];
    for (int i = 0; i < 64; ++i) r[i] = 0;
    modL(r, x);
}

/* ---------- 点解码（unpackneg） ----------
 * 从 32 字节编码恢复点，返回其相反数（-A）。
 * 用于验证流程中的 [k](-A)。
 * 成功返回 0；失败返回 -1。 */
static int unpackneg(gf r[4], const u8 p[32])
{
    gf t, chk, num, den, den2, den4, den6;

    set25519(r[2], gf1);
    unpack25519(r[1], p);

    S(num, r[1]);           /* num = y^2 */
    M(den, num, D);         /* den = d*y^2 */
    Z(num, num, r[2]);      /* num = y^2 - 1 */
    A(den, r[2], den);      /* den = d*y^2 + 1 */

    S(den2, den);
    S(den4, den2);
    M(den6, den4, den2);    /* den6 = den^6 */
    M(t, den6, num);        /* t = num * den^6 */
    M(t, t, den);           /* t = num * den^7 */

    pow2523(t, t);          /* t = (num*den^7)^((p-5)/8) */
    M(t, t, num);           /* t *= num */
    M(t, t, den);           /* t *= den */
    M(t, t, den);           /* t *= den (再次) */
    M(r[0], t, den);        /* x = t * den */

    S(chk, r[0]);
    M(chk, chk, den);       /* chk = x^2 * den */
    if (neq25519(chk, num)) {
        M(r[0], r[0], I);   /* x *= sqrt(-1) */
    }

    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) return -1;

    /* 符号修正 */
    if (par25519(r[0]) == (p[31] >> 7)) {
        Z(r[0], gf0, r[0]);
    }

    M(r[3], r[0], r[1]);    /* T = X*Y */
    return 0;
}

/* ============================================================
 * 公共 API
 * ============================================================ */

int ed25519_keypair_from_seed(const uint8_t seed[ED25519_SEED_LEN],
                              uint8_t priv[ED25519_PRIVKEY_LEN],
                              uint8_t pub[ED25519_PUBKEY_LEN])
{
    if (!seed || !priv || !pub) return -1;

    u8 d[64];
    gf p[4];
    struct sha512_ctx ctx;

    sha512_init(&ctx);
    sha512_update(&ctx, seed, 32);
    sha512_final(&ctx, d);

    d[0]  &= 248;
    d[31] &= 127;
    d[31] |= 64;

    scalarbase(p, d);
    pack(pub, p);

    for (int i = 0; i < 32; ++i) priv[i]      = seed[i];
    for (int i = 0; i < 32; ++i) priv[32 + i] = pub[i];

    mem_zero(d, 64);
    return 0;
}

int ed25519_sign(const uint8_t priv[ED25519_PRIVKEY_LEN],
                 const void *msg, uint64_t msg_len,
                 uint8_t sig[ED25519_SIG_LEN])
{
    if (!priv || !sig) return -1;
    if (!msg && msg_len > 0) return -1;

    u8 d[64], h[64], r[64];
    i64 x[64];
    gf p[4];
    struct sha512_ctx ctx;

    /* d = SHA-512(seed)，clamp 后低 32 字节为秘密标量 a */
    sha512_init(&ctx);
    sha512_update(&ctx, priv, 32);
    sha512_final(&ctx, d);
    d[0]  &= 248;
    d[31] &= 127;
    d[31] |= 64;

    /* r = SHA-512(d[32..64] || msg) mod L */
    sha512_init(&ctx);
    sha512_update(&ctx, d + 32, 32);
    if (msg_len > 0) sha512_update(&ctx, msg, msg_len);
    sha512_final(&ctx, r);
    reduce(r);

    /* R = [r]B，写入 sig[0..32] */
    scalarbase(p, r);
    pack(sig, p);

    /* k = SHA-512(R || A || msg) mod L */
    sha512_init(&ctx);
    sha512_update(&ctx, sig, 32);
    sha512_update(&ctx, priv + 32, 32);
    if (msg_len > 0) sha512_update(&ctx, msg, msg_len);
    sha512_final(&ctx, h);
    reduce(h);

    /* S = (r + k * a) mod L，写入 sig[32..64] */
    for (int i = 0; i < 64; ++i) x[i] = 0;
    for (int i = 0; i < 32; ++i) x[i] = (u64)r[i];
    for (int i = 0; i < 32; ++i) {
        for (int j = 0; j < 32; ++j) {
            x[i + j] += (i64)h[i] * (u64)d[j];
        }
    }
    modL(sig + 32, x);

    mem_zero(d, 64);
    mem_zero(h, 64);
    mem_zero(r, 64);
    return 0;
}

int ed25519_verify(const uint8_t pub[ED25519_PUBKEY_LEN],
                   const void *msg, uint64_t msg_len,
                   const uint8_t sig[ED25519_SIG_LEN])
{
    if (!pub || !sig) return 0;
    if (!msg && msg_len > 0) return 0;

    u8 t[32], h[64];
    gf p[4], q[4];
    struct sha512_ctx ctx;

    /* 解码 A' = -A */
    if (unpackneg(q, pub)) return 0;

    /* k = SHA-512(R || A || msg) mod L */
    sha512_init(&ctx);
    sha512_update(&ctx, sig, 32);
    sha512_update(&ctx, pub, 32);
    if (msg_len > 0) sha512_update(&ctx, msg, msg_len);
    sha512_final(&ctx, h);
    reduce(h);

    /* p = [k](-A) */
    scalarmult(p, q, h);

    /* q = [S]B */
    scalarbase(q, sig + 32);

    /* p = [k](-A) + [S]B = [S]B - [k]A
     * 应等于 R。 */
    add(p, q);
    pack(t, p);

    /* 常量时间比较：相等返回 1 */
    return ct_verify_32(sig, t);
}
/*===OmniBridgeOs/kernel/arch/x64/ed25519.c 结束===*/