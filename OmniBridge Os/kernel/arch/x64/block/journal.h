/*===OmniBridgeOs/kernel/arch/x64/block/journal.h===*/
#ifndef OMNIBRIDGE_BLOCK_JOURNAL_H
#define OMNIBRIDGE_BLOCK_JOURNAL_H

#include "block.h"

#define JNL_MAGIC        0x4A524E4Cu
#define JNL_COMMIT_MAGIC 0x4A434D54u
#define JNL_MAX_TXN_BLOCKS 64

struct journal_header {
    uint32_t magic;
    uint32_t seq;
    uint32_t count;
    uint32_t checksum;
    uint32_t commit;
    uint32_t _pad;
} __attribute__((packed));

struct journal_entry {
    uint64_t target_lba;
    uint64_t _pad;
    uint8_t  data[4096];
} __attribute__((packed));

struct journal_txn {
    struct journal_header  header;
    struct journal_entry   entries[JNL_MAX_TXN_BLOCKS];
    uint32_t               used;
};

struct journal {
    struct block_device *dev;
    uint64_t             start_block;
    uint32_t             size_blocks;
    uint32_t             head;
    spinlock_t           lock;
    int                  ready;
    int                  active;
};

void journal_init(void);
int  journal_open(struct journal *j, struct block_device *dev,
                  uint64_t start_block, uint32_t size_blocks);
struct journal_txn *journal_begin(struct journal *j);
int  journal_add(struct journal_txn *txn, uint64_t target_block,
                 const void *data);
int  journal_commit(struct journal *j, struct journal_txn *txn);
void journal_abort(struct journal *j, struct journal_txn *txn);
int  journal_recover(struct journal *j);

/* ★ P1：崩溃注入（仅用于测试）。
 *
 * 语义：
 *   - 全局变量 g_jnl_crash_stage 表示"在第 N 步崩溃"。
 *   - 0 表示不崩溃（默认）；运行期通过 journal_set_crash_stage(N) 设置。
 *   - 在 journal_commit 的关键阶段前后检查，命中则 hlt。
 *
 * 阶段号：
 *   1 = 写完日志头（尚未提交）后
 *   2 = 写完日志数据块后
 *   3 = 写完 commit 标记后
 *   4 = 部分应用到主位置后
 */
void journal_set_crash_stage(int stage);
int  journal_get_crash_stage(void);

#endif /* OMNIBRIDGE_BLOCK_JOURNAL_H */