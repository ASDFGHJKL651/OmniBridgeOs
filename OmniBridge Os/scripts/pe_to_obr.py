#!/usr/bin/env python3
# OmniBridgeOs/scripts/pe_to_obr.py
"""
PE32+ (x86-64) -> OmniBridge .obr 转换器。

用法：
    pe_to_obr.py <input.pe> <output.obr> [--minpriv N] [--entry-symbol NAME]

输出符合规格书 §13：
    [ obr_header (80) | obr_phdr[N] (56*N) | 节数据（0x1000 对齐） ]

关键约定（人工必须审查）：
    - obr_header.entry_point 是"文件内偏移"，不是 VMA。
      因为 UEL 内部计算 load_base + entry_point，若填成 VMA 则入口错位。
    - 每个 PT_LOAD 的 vaddr 来自 PE 节的 RVA（相对镜像基址），
      UEL 加载到 load_base + vaddr。
    - 本步不生成节头表（sh_offset=0, sh_count=0）。
    - 本步不写签名（sig_type=0）；签名由内核侧 OB_InternalSign 追加。
"""

import argparse
import struct
import sys

# ---- PE 常量 ----
IMAGE_SCN_MEM_DISCARDABLE = 0x02000000
IMAGE_SCN_MEM_EXECUTE     = 0x20000000
IMAGE_SCN_MEM_READ        = 0x40000000
IMAGE_SCN_MEM_WRITE       = 0x80000000

# ---- OBR 常量（与 kernel/arch/x64/obr.h 严格一致）----
OBR_MAGIC      = 0x4F425220
OBR_VERSION    = 0x0100
OBR_ARCH_X64   = 0x01

OBR_PT_LOAD    = 1
OBR_PF_X       = 0x1
OBR_PF_W       = 0x2
OBR_PF_R       = 0x4

FILE_ALIGN     = 0x1000
OBR_HDR_SIZE   = 80
OBR_PHDR_SIZE  = 56


def read_pe(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"MZ":
        raise ValueError("not a PE: missing MZ")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise ValueError("not a PE: missing PE signature")
    coff_off = e_lfanew + 4
    machine, num_sections = struct.unpack_from("<HH", data, coff_off)
    if machine != 0x8664:
        raise ValueError(f"not x64 PE: machine=0x{machine:04x}")
    size_opt = struct.unpack_from("<H", data, coff_off + 16)[0]
    opt_off  = coff_off + 20
    magic = struct.unpack_from("<H", data, opt_off)[0]
    if magic != 0x20B:
        raise ValueError(f"not PE32+: magic=0x{magic:04x}")
    entry_rva  = struct.unpack_from("<I", data, opt_off + 16)[0]
    image_base = struct.unpack_from("<Q", data, opt_off + 24)[0]
    size_image = struct.unpack_from("<I", data, opt_off + 56)[0]

    sect_off = opt_off + size_opt
    sections = []
    for i in range(num_sections):
        off = sect_off + i * 40
        name         = data[off:off + 8].rstrip(b"\0").decode("ascii", "replace")
        virtual_size = struct.unpack_from("<I", data, off + 8)[0]
        virtual_addr = struct.unpack_from("<I", data, off + 12)[0]
        size_raw     = struct.unpack_from("<I", data, off + 16)[0]
        ptr_raw      = struct.unpack_from("<I", data, off + 20)[0]
        char         = struct.unpack_from("<I", data, off + 36)[0]
        sections.append({
            "name":     name,
            "vsize":    virtual_size,
            "vaddr":    virtual_addr,
            "size_raw": size_raw,
            "ptr_raw":  ptr_raw,
            "char":     char,
        })
    return {
        "data":       data,
        "entry_rva":  entry_rva,
        "image_base": image_base,
        "size_image": size_image,
        "sections":   sections,
    }


def section_flags(char):
    flags = 0
    if char & IMAGE_SCN_MEM_EXECUTE: flags |= OBR_PF_X
    if char & IMAGE_SCN_MEM_WRITE:   flags |= OBR_PF_W
    if char & IMAGE_SCN_MEM_READ:    flags |= OBR_PF_R
    if flags == 0:
        flags = OBR_PF_R
    return flags


def convert(inp, outp, minpriv, dep_names):
    pe = read_pe(inp)

    # 1) 挑选要加载的节：丢弃 DISCARDABLE，保留 vsize>0 或 size_raw>0
    keep = []
    for s in pe["sections"]:
        if s["vsize"] == 0 and s["size_raw"] == 0:
            continue
        if s["char"] & IMAGE_SCN_MEM_DISCARDABLE:
            continue
        keep.append(s)
    keep.sort(key=lambda s: s["vaddr"])

    if not keep:
        raise ValueError("no loadable sections")

    # 2) 头部布局
    ph_count  = len(keep)
    ph_offset = OBR_HDR_SIZE
    data_off  = OBR_HDR_SIZE + ph_count * OBR_PHDR_SIZE
    data_off  = (data_off + FILE_ALIGN - 1) & ~(FILE_ALIGN - 1)

    # 3) 逐节分配输出偏移，同时构造 phdr 列表
    phs      = []
    payload  = bytearray()
    entry_ph = None
    entry_rva = pe["entry_rva"]

    for s in keep:
        rva    = s["vaddr"]
        filesz = s["size_raw"]
        memsz  = max(s["vsize"], filesz)
        if memsz == 0:
            memsz = filesz
        flags  = section_flags(s["char"])
        cur_off = data_off + len(payload)

        phs.append({
            "type":   OBR_PT_LOAD,
            "flags":  flags,
            "offset": cur_off,
            "vaddr":  rva,
            "filesz": filesz,
            "memsz":  memsz,
            "align":  FILE_ALIGN,
        })

        if rva <= entry_rva < rva + max(s["vsize"], filesz):
            entry_ph = (s, cur_off)

        if filesz > 0:
            payload += pe["data"][s["ptr_raw"]:s["ptr_raw"] + filesz]
        pad = (-len(payload)) % FILE_ALIGN
        payload += b"\0" * pad

        print(f"[pe_to_obr] SECTION {s['name']:<8} "
              f"rva=0x{rva:08x} filesz=0x{filesz:x} memsz=0x{memsz:x} "
              f"flags=0x{flags:x}", flush=True)

    # 4) 计算 entry_point（文件内偏移）
    if entry_ph is None:
        raise ValueError(
            f"entry_rva=0x{entry_rva:x} not inside any kept section")
    sect, sect_file_off = entry_ph
    entry_point = sect_file_off + (entry_rva - sect["vaddr"])

    print(f"[pe_to_obr] PE entry_rva=0x{entry_rva:x} "
          f"-> entry_point=0x{entry_point:x} (in {sect['name']})",
          flush=True)

    # 5) 追加依赖字符串表（'\0' 分隔）
    dep_count = 0
    dep_strings_offset = 0
    if dep_names:
        dep_strings_offset = data_off + len(payload)
        for d in dep_names:
            if isinstance(d, str):
                payload += d.encode("utf-8") + b"\0"
            else:
                payload += bytes(d) + b"\0"
            dep_count += 1
        print(f"[pe_to_obr] appended {dep_count} dependency names "
              f"at file offset 0x{dep_strings_offset:x}", flush=True)

    # 6) 构造 obr_header（packed 80 字节）
    hdr = bytearray(OBR_HDR_SIZE)
    struct.pack_into("<I", hdr, 0,  OBR_MAGIC)
    struct.pack_into("<H", hdr, 4,  OBR_VERSION)
    hdr[6] = OBR_ARCH_X64
    hdr[7] = minpriv & 0xFF
    struct.pack_into("<Q", hdr, 8,  entry_point)
    struct.pack_into("<Q", hdr, 16, ph_offset)
    struct.pack_into("<H", hdr, 24, ph_count)
    struct.pack_into("<H", hdr, 26, 0)             # _pad0
    struct.pack_into("<Q", hdr, 28, 0)             # sh_offset
    struct.pack_into("<H", hdr, 36, 0)             # sh_count
    struct.pack_into("<H", hdr, 38, 0)             # _pad1
    struct.pack_into("<Q", hdr, 40, dep_count)             # dep_count
    struct.pack_into("<Q", hdr, 48, dep_strings_offset)    # dep_strings_offset
    struct.pack_into("<I", hdr, 56, 0)             # checksum
    hdr[60] = 0x00                                  # sig_type
    hdr[61] = 0; hdr[62] = 0; hdr[63] = 0           # sig_padding
    struct.pack_into("<I", hdr, 64, 0)             # sig_length

    # 7) 构造 phdr 表（packed 56 字节）
    phbuf = bytearray()
    for ph in phs:
        p = bytearray(OBR_PHDR_SIZE)
        struct.pack_into("<I", p, 0,  ph["type"])
        struct.pack_into("<I", p, 4,  ph["flags"])
        struct.pack_into("<Q", p, 8,  ph["offset"])
        struct.pack_into("<Q", p, 16, ph["vaddr"])
        struct.pack_into("<Q", p, 24, ph["filesz"])
        struct.pack_into("<Q", p, 32, ph["memsz"])
        struct.pack_into("<Q", p, 40, ph["align"])
        phbuf += p

    # 8) 写出
    with open(outp, "wb") as f:
        f.write(hdr)
        f.write(phbuf)
        gap = data_off - (OBR_HDR_SIZE + len(phbuf))
        if gap > 0:
            f.write(b"\0" * gap)
        f.write(payload)

    total = data_off + len(payload)
    print(f"[pe_to_obr] wrote {outp}: {ph_count} PT_LOAD, "
          f"deps={dep_count}, size=0x{total:x}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--minpriv", type=int, default=0)
    ap.add_argument("--entry-symbol", default="_start",
                    help="仅为日志使用；entry_rva 直接取自 PE 头")
    ap.add_argument("--dep", action="append", default=[],
                    help="依赖库名（可多次指定）")
    args = ap.parse_args()
    if not (0 <= args.minpriv <= 9):
        print("[pe_to_obr] minpriv must be 0..9", file=sys.stderr)
        sys.exit(1)
    convert(args.input, args.output, args.minpriv, args.dep)


if __name__ == "__main__":
    main()