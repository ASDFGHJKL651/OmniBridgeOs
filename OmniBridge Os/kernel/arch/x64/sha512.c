/* kernel/arch/x64/sha512.c
 * FIPS 180-4 SHA-512 自实现。
 */
#include "sha512.h"

/* 轮常量（FIPS 180-4 §4.2.3）—— 与 sha384.c 相同 */
static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL,
    0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL,
    0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL,
    0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL,
    0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL,
    0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL,
    0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL,
    0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL,
    0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL,
    0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL,
    0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

/* SHA-512 初始 IV（FIPS 180-4 §5.3.3） */
static const uint64_t IV512[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
    0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
};

#define ROTR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x)     (ROTR64(x, 28) ^ ROTR64(x, 34) ^ ROTR64(x, 39))
#define BSIG1(x)     (ROTR64(x, 14) ^ ROTR64(x, 18) ^ ROTR64(x, 41))
#define SSIG0(x)     (ROTR64(x, 1)  ^ ROTR64(x, 8)  ^ ((x) >> 7))
#define SSIG1(x)     (ROTR64(x, 19) ^ ROTR64(x, 61) ^ ((x) >> 6))

void sha512_init(struct sha512_ctx *ctx)
{
    for (int i = 0; i < 8; ++i) ctx->state[i] = IV512[i];
    ctx->bitlen[0] = 0;
    ctx->bitlen[1] = 0;
    ctx->buffer_len = 0;
    for (unsigned i = 0; i < sizeof(ctx->buffer); ++i) ctx->buffer[i] = 0;
}

static void sha512_process_block(struct sha512_ctx *ctx, const uint8_t *block)
{
    uint64_t w[80];
    uint64_t a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; ++i) {
        w[i] = ((uint64_t)block[i * 8 + 0] << 56) |
               ((uint64_t)block[i * 8 + 1] << 48) |
               ((uint64_t)block[i * 8 + 2] << 40) |
               ((uint64_t)block[i * 8 + 3] << 32) |
               ((uint64_t)block[i * 8 + 4] << 24) |
               ((uint64_t)block[i * 8 + 5] << 16) |
               ((uint64_t)block[i * 8 + 6] << 8)  |
               ((uint64_t)block[i * 8 + 7]);
    }
    for (i = 16; i < 80; ++i) {
        w[i] = SSIG1(w[i - 2]) + w[i - 7] + SSIG0(w[i - 15]) + w[i - 16];
    }

    a = ctx->state[0]; b = ctx->state[1];
    c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5];
    g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 80; ++i) {
        uint64_t t1 = h + BSIG1(e) + CH(e, f, g) + K512[i] + w[i];
        uint64_t t2 = BSIG0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b;
    ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f;
    ctx->state[6] += g; ctx->state[7] += h;
}

void sha512_update(struct sha512_ctx *ctx, const void *data, uint64_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    uint64_t old_lo = ctx->bitlen[0];
    ctx->bitlen[0] += len << 3;
    if (ctx->bitlen[0] < old_lo) ctx->bitlen[1]++;
    ctx->bitlen[1] += len >> 61;

    while (len > 0) {
        uint32_t space = 128u - ctx->buffer_len;
        uint32_t copy  = (len < (uint64_t)space) ? (uint32_t)len : space;

        for (uint32_t i = 0; i < copy; ++i) {
            ctx->buffer[ctx->buffer_len + i] = p[i];
        }
        ctx->buffer_len += copy;
        p   += copy;
        len -= copy;

        if (ctx->buffer_len == 128u) {
            sha512_process_block(ctx, ctx->buffer);
            ctx->buffer_len = 0;
        }
    }
}

void sha512_final(struct sha512_ctx *ctx, uint8_t out[64])
{
    uint64_t bitlen_lo = ctx->bitlen[0];
    uint64_t bitlen_hi = ctx->bitlen[1];
    uint32_t i;

    ctx->buffer[ctx->buffer_len++] = 0x80u;

    if (ctx->buffer_len > 112u) {
        while (ctx->buffer_len < 128u) {
            ctx->buffer[ctx->buffer_len++] = 0;
        }
        sha512_process_block(ctx, ctx->buffer);
        ctx->buffer_len = 0;
    }

    while (ctx->buffer_len < 112u) {
        ctx->buffer[ctx->buffer_len++] = 0;
    }

    for (i = 0; i < 8; ++i) {
        ctx->buffer[112 + i] = (uint8_t)(bitlen_hi >> (56 - i * 8));
    }
    for (i = 0; i < 8; ++i) {
        ctx->buffer[120 + i] = (uint8_t)(bitlen_lo >> (56 - i * 8));
    }

    sha512_process_block(ctx, ctx->buffer);
    ctx->buffer_len = 0;

    /* 全部 8 个 64 位状态字都输出 */
    for (i = 0; i < 8; ++i) {
        out[i * 8 + 0] = (uint8_t)(ctx->state[i] >> 56);
        out[i * 8 + 1] = (uint8_t)(ctx->state[i] >> 48);
        out[i * 8 + 2] = (uint8_t)(ctx->state[i] >> 40);
        out[i * 8 + 3] = (uint8_t)(ctx->state[i] >> 32);
        out[i * 8 + 4] = (uint8_t)(ctx->state[i] >> 24);
        out[i * 8 + 5] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 8 + 6] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 8 + 7] = (uint8_t)(ctx->state[i]);
    }
}

void sha512(const void *data, uint64_t len, uint8_t out[64])
{
    struct sha512_ctx ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, data, len);
    sha512_final(&ctx, out);
}