#!/usr/bin/env python3
#===OmniBridgeOs/scripts/mkfs_obfs.py===
"""
宿主侧 OBFS-RW 镜像格式化工具。

用法：
    python3 scripts/mkfs_obfs.py <image> [--size-mb 64] [--inodes 256]

生成的镜像可以直接给 QEMU 使用：
    -drive file=<image>,if=none,id=hd0,format=raw
    -device virtio-blk-pci,drive=hd0

布局（与 kernel/arch/x64/block/mkfs.c 严格一致）：
    block 0                        : superblock
    block 1                        : block bitmap
    block 2                        : inode bitmap
    block 3 .. 3+T-1               : inode table
    block 3+T .. 3+T+J-1           : journal
    block 3+T+J .. end             : data blocks
"""

import argparse
import os
import struct
import sys

OBFS_MAGIC       = 0x4F424653
PAGE_SIZE        = 4096
OBFS_INODE_SIZE  = 256
OBFS_RW_INODE_SIZE = 384
JOURNAL_SIZE     = 32

VFS_S_IFDIR      = 0x4000


def build_superblock(total_blocks, inode_count, inode_table_start,
                     data_block_start, journal_start):
    buf = bytearray(PAGE_SIZE)
    struct.pack_into("<I", buf, 0, OBFS_MAGIC)
    struct.pack_into("<I", buf, 4, PAGE_SIZE)
    struct.pack_into("<Q", buf, 8, total_blocks)
    struct.pack_into("<Q", buf, 16, inode_count)
    struct.pack_into("<Q", buf, 24, 0)                 # root_inode
    # ita_public_key[32] @ 32..63 保持为 0
    struct.pack_into("<Q", buf, 64, 1)                 # block_bitmap_start
    struct.pack_into("<Q", buf, 72, 2)                 # inode_bitmap_start
    struct.pack_into("<Q", buf, 80, inode_table_start)
    struct.pack_into("<Q", buf, 88, data_block_start)

    # 扩展 rw 字段
    base = 96
    struct.pack_into("<I", buf, base + 0, 1)                    # rw_version
    struct.pack_into("<I", buf, base + 4, journal_start)
    struct.pack_into("<I", buf, base + 8, JOURNAL_SIZE)
    struct.pack_into("<I", buf, base + 12, 0)                   # fsck_state
    struct.pack_into("<Q", buf, base + 16, 0)                   # default_quota
    buf[base + 24] = 0                                          # crypto_algo
    return buf


def build_block_bitmap(used_blocks):
    buf = bytearray(PAGE_SIZE)
    for i in range(used_blocks):
        buf[i >> 3] |= (1 << (i & 7))
    return buf


def build_inode_bitmap():
    buf = bytearray(PAGE_SIZE)
    buf[0] |= 0x01
    return buf


def build_root_inode():
    buf = bytearray(OBFS_RW_INODE_SIZE)
    # struct obfs_inode base（256 字节）
    #   mode(2) links(2) uid(4) gid(4) size(8) atime(8) mtime(8) ctime(8)
    #   direct[12](48) indirect1(4) indirect2(4) reserved[156]
    struct.pack_into("<H", buf, 0, VFS_S_IFDIR | 0o755)   # mode
    struct.pack_into("<H", buf, 2, 1)                     # links
    # size/uid/gid = 0
    # direct[] = 0
    # rw 扩展字段
    struct.pack_into("<Q", buf, 256, 0)                   # quota_limit
    struct.pack_into("<Q", buf, 264, 0)                   # parent_ino
    struct.pack_into("<I", buf, 272, 1)                   # link_count
    struct.pack_into("<I", buf, 276, 0)                   # _pad
    # reserved[104] 保持为 0
    return buf


def inode_table_blocks(inode_count):
    total = inode_count * OBFS_RW_INODE_SIZE
    return (total + PAGE_SIZE - 1) // PAGE_SIZE


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--size-mb", type=int, default=64)
    ap.add_argument("--inodes", type=int, default=256)
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    if os.path.exists(args.image) and not args.force:
        print(f"[mkfs] {args.image} exists; use --force to overwrite",
              file=sys.stderr)
        return 1

    size_bytes = args.size_mb * 1024 * 1024
    total_blocks = size_bytes // PAGE_SIZE
    if total_blocks < 64:
        print("[mkfs] size too small", file=sys.stderr)
        return 1

    T = inode_table_blocks(args.inodes)
    journal_start = 3 + T
    data_block_start = journal_start + JOURNAL_SIZE

    if data_block_start >= total_blocks:
        print("[mkfs] size too small for chosen inode count",
              file=sys.stderr)
        return 1

    print(f"[mkfs] image={args.image} size={args.size_mb}MB "
          f"inodes={args.inodes} T={T} J={JOURNAL_SIZE} "
          f"data_start={data_block_start}")

    with open(args.image, "wb") as f:
        # block 0
        f.write(build_superblock(total_blocks, args.inodes,
                                 inode_table_start=3,
                                 data_block_start=data_block_start,
                                 journal_start=journal_start))
        # block 1
        f.write(build_block_bitmap(data_block_start))
        # block 2
        f.write(build_inode_bitmap())

        # block 3 .. 3+T-1：inode 表
        root = build_root_inode()
        f.write(root)                                  # inode 0
        remaining = T * PAGE_SIZE - OBFS_RW_INODE_SIZE
        f.write(b"\x00" * remaining)

        # 日志区：全部清零
        f.write(b"\x00" * (JOURNAL_SIZE * PAGE_SIZE))

        # 数据块区：稀疏文件，直接 seek 到末尾
        f.seek(size_bytes - 1)
        f.write(b"\x00")

    print(f"[mkfs] done: {args.image}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
#===OmniBridgeOs/scripts/mkfs_obfs.py 结束===