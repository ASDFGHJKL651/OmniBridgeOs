/*===OmniBridgeOs/kernel/arch/x64/block/journal.c===*/
/*
 * OBFS 日志（第 18B 步 P1，修正版）。
 *
 * ★ 关键修正（对照上一版）：
 *   struct journal_txn 大小 ≈ 257 KB（64 × 4KB entry），
 *   远超 kmalloc 上限（PAGE_SIZE = 4 KB），原 kzalloc 必然返回 NULL，
 *   导致所有 create/mkdir/write 返回 -EAGAIN。
 *
 *   修复：使用进程内静态池 g_txn（单事务，j->active 保证互斥），
 *   彻底移除 journal 路径上的动态内存分配。
 */
#include "journal.h"
#include "page_cache.h"
#include "serial.h"
#include "pmm.h"

#define JNL_BLOCK_SECTORS (PAGE_SIZE / BLOCK_SECTOR_SIZE)

static int g_jnl_crash_stage = 0;

/* ★ 静态事务池（约 257 KB，位于 .bss）。
 *   j->active 保证同一时刻只有一个事务占用它。 */
static struct journal_txn g_txn;

void journal_set_crash_stage(int stage) { g_jnl_crash_stage = stage; }
int  journal_get_crash_stage(void)      { return g_jnl_crash_stage; }

static void jnl_crash_if(int stage)
{
    if (g_jnl_crash_stage == stage) {
        serial_printf("[JOURNAL] CRASH INJECT at stage %d\n", stage);
        for (;;) {
            __asm__ __volatile__("cli; hlt" ::: "memory");
        }
    }
}

static void jnl_zero(void *p, uint64_t n)
{
    uint8_t *b = (uint8_t *)p;
    for (uint64_t i = 0; i < n; ++i) b[i] = 0;
}

void journal_init(void)
{
    g_jnl_crash_stage = 0;
    jnl_zero(&g_txn, sizeof(g_txn));
    serial_printf("[JOURNAL] init: max_txn_blocks=%u entry_size=%u\n",
                  (unsigned)JNL_MAX_TXN_BLOCKS,
                  (unsigned)sizeof(struct journal_entry));
}

int journal_open(struct journal *j, struct block_device *dev,
                 uint64_t start_block, uint32_t size_blocks)
{
    if (!j || !dev) return OB_EINVAL;
    if (size_blocks < 2 || size_blocks > 4096) return OB_EINVAL;

    j->dev         = dev;
    j->start_block = start_block;
    j->size_blocks = size_blocks;
    j->head        = 0;
    spin_lock_init(&j->lock);
    j->ready       = 1;
    j->active      = 0;

    serial_printf("[JOURNAL] open: start_block=%llu size=%u\n",
                  (unsigned long long)start_block, (unsigned)size_blocks);
    return 0;
}

struct journal_txn *journal_begin(struct journal *j)
{
    if (!j || !j->ready) return 0;
    if (j->active) return 0;

    /* 只清零 header 与 used；entries 无需清零（每次 add 全量写） */
    jnl_zero(&g_txn.header, sizeof(g_txn.header));
    g_txn.used = 0;
    j->active  = 1;
    return &g_txn;
}

int journal_add(struct journal_txn *txn, uint64_t target_block,
                const void *data)
{
    if (!txn || !data) return OB_EINVAL;
    if (txn->used >= JNL_MAX_TXN_BLOCKS) return OB_ENOMEM;

    struct journal_entry *e = &txn->entries[txn->used];
    e->target_lba = target_block;
    const uint8_t *s = (const uint8_t *)data;
    for (uint32_t i = 0; i < PAGE_SIZE; ++i) e->data[i] = s[i];
    txn->used++;
    return 0;
}

static uint32_t jnl_checksum(const struct journal_header *h,
                             const struct journal_entry *entries,
                             uint32_t count)
{
    uint32_t sum = 0x811C9DC5u;
    const uint8_t *p = (const uint8_t *)h;
    for (size_t i = 0; i < sizeof(*h); ++i) {
        sum ^= p[i];
        sum *= 16777619u;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *ep = (const uint8_t *)&entries[i];
        for (size_t k = 0; k < sizeof(entries[i]); ++k) {
            sum ^= ep[k];
            sum *= 16777619u;
        }
    }
    return sum;
}

int journal_commit(struct journal *j, struct journal_txn *txn)
{
    if (!j || !txn || !j->ready) return OB_EINVAL;
    if (txn->used == 0) {
        journal_abort(j, txn);
        return 0;
    }

    struct journal_header h;
    h.magic    = JNL_MAGIC;
    h.seq      = j->head + 1;
    h.count    = txn->used;
    h.checksum = jnl_checksum(&h, txn->entries, txn->used);
    h.commit   = 0;
    h._pad     = 0;

    uint64_t hdr_block = j->start_block;
    int rc = block_write(j->dev, hdr_block * JNL_BLOCK_SECTORS,
                         JNL_BLOCK_SECTORS, &h);
    if (rc != 0) { journal_abort(j, txn); return rc; }

    jnl_crash_if(1);

    for (uint32_t i = 0; i < txn->used; ++i) {
        uint64_t blk = j->start_block + 1 + i;
        if (blk >= j->start_block + j->size_blocks) {
            journal_abort(j, txn);
            return OB_ENOMEM;
        }
        rc = block_write(j->dev, blk * JNL_BLOCK_SECTORS,
                         JNL_BLOCK_SECTORS, &txn->entries[i]);
        if (rc != 0) { journal_abort(j, txn); return rc; }
    }

    jnl_crash_if(2);

    h.commit   = 1;
    h.checksum = jnl_checksum(&h, txn->entries, txn->used);
    rc = block_write(j->dev, hdr_block * JNL_BLOCK_SECTORS,
                     JNL_BLOCK_SECTORS, &h);
    if (rc != 0) { journal_abort(j, txn); return rc; }

    jnl_crash_if(3);

    for (uint32_t i = 0; i < txn->used; ++i) {
        uint64_t blk = txn->entries[i].target_lba;
        block_write(j->dev, blk * JNL_BLOCK_SECTORS,
                    JNL_BLOCK_SECTORS, txn->entries[i].data);
        pcache_invalidate(j->dev, blk);
        if (i == 0) jnl_crash_if(4);
    }

    struct journal_header clean;
    jnl_zero(&clean, sizeof(clean));
    block_write(j->dev, hdr_block * JNL_BLOCK_SECTORS,
                JNL_BLOCK_SECTORS, &clean);

    j->head = (j->head + txn->used + 1) % (j->size_blocks - 1);

    /* ★ 不再 kfree：txn 是静态池成员 */
    j->active = 0;
    return 0;
}

void journal_abort(struct journal *j, struct journal_txn *txn)
{
    if (!j) return;
    /* ★ 不再 kfree：txn 是静态池成员 */
    (void)txn;
    j->active = 0;
}

int journal_recover(struct journal *j)
{
    if (!j || !j->ready) return OB_EINVAL;

    struct journal_header h;
    int rc = block_read(j->dev, j->start_block * JNL_BLOCK_SECTORS,
                        JNL_BLOCK_SECTORS, &h);
    if (rc != 0) return rc;

    if (h.magic != JNL_MAGIC) {
        serial_printf("[JOURNAL] replay done (no active txn)\n");
        return 0;
    }
    if (h.commit != 1) {
        serial_printf("[JOURNAL] discard incomplete txn seq=%u\n",
                      (unsigned)h.seq);
        struct journal_header clean;
        jnl_zero(&clean, sizeof(clean));
        block_write(j->dev, j->start_block * JNL_BLOCK_SECTORS,
                    JNL_BLOCK_SECTORS, &clean);
        serial_printf("[JOURNAL] replay done (discarded)\n");
        return 0;
    }
    if (h.count == 0 || h.count > JNL_MAX_TXN_BLOCKS) {
        serial_printf("[JOURNAL] bad count=%u\n", (unsigned)h.count);
        return OB_EIO;
    }

    serial_printf("[JOURNAL] replaying txn seq=%u count=%u\n",
                  (unsigned)h.seq, (unsigned)h.count);

    for (uint32_t i = 0; i < h.count; ++i) {
        uint64_t blk = j->start_block + 1 + i;
        struct journal_entry e;
        rc = block_read(j->dev, blk * JNL_BLOCK_SECTORS,
                        JNL_BLOCK_SECTORS, &e);
        if (rc != 0) return rc;

        uint64_t target = e.target_lba;
        rc = block_write(j->dev, target * JNL_BLOCK_SECTORS,
                         JNL_BLOCK_SECTORS, e.data);
        if (rc != 0) {
            serial_printf("[JOURNAL] replay failed at target=%llu\n",
                          (unsigned long long)target);
            return rc;
        }
        pcache_invalidate(j->dev, target);
    }

    struct journal_header clean;
    jnl_zero(&clean, sizeof(clean));
    block_write(j->dev, j->start_block * JNL_BLOCK_SECTORS,
                JNL_BLOCK_SECTORS, &clean);

    serial_printf("[JOURNAL] replay done (seq=%u blocks=%u)\n",
                  (unsigned)h.seq, (unsigned)h.count);
    return 0;
}
/*===OmniBridgeOs/kernel/arch/x64/block/journal.c 结束===*/