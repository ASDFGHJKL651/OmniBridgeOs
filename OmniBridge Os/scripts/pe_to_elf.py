#!/usr/bin/env python3
"""
PE32+ (x86-64) -> ELF64 executable converter for OmniBridge kernel.

用法：
  pe_to_elf.py <input.pe> <output.elf> <lma_base> <vma_base>

约定：
  - 输入是 lld-link 输出的 PE32+ (x86-64)
  - 每个 vsize > 0 的 PE Section 映射为一个 PT_LOAD
  - ELF 的：
        p_vaddr = vma_base + RVA
        p_paddr = lma_base + RVA
        p_filesz = size_raw        （文件中实际字节）
        p_memsz  = max(vsize, size_raw)  （BSS 由 ELF loader 补零）
  - Section 按 RVA 排序，保证 PT_LOAD 的文件偏移单调递增
"""

import struct
import sys

# 常量
IMAGE_SCN_MEM_DISCARDABLE = 0x02000000
IMAGE_SCN_MEM_EXECUTE     = 0x20000000
IMAGE_SCN_MEM_READ        = 0x40000000
IMAGE_SCN_MEM_WRITE       = 0x80000000

FILE_ALIGN = 0x1000        # PE/ELF 数据页对齐


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
            "name":   name,
            "vsize":  virtual_size,
            "vaddr":  virtual_addr,
            "size_raw": size_raw,
            "ptr_raw":  ptr_raw,
            "char":   char,
        })

    return {
        "data":       data,
        "entry_rva":  entry_rva,
        "image_base": image_base,
        "size_image": size_image,
        "sections":   sections,
    }


def write_elf(pe, out_path, lma_base, vma_base):
    # 1) 挑选要加载的 section
    #    丢弃 MEM_DISCARDABLE（调试、重定位、异常信息等）
    #    保留 vsize > 0，包括 .bss（size_raw=0）
    keep = []
    for s in pe["sections"]:
        if s["vsize"] == 0 and s["size_raw"] == 0:
            continue
        if s["char"] & IMAGE_SCN_MEM_DISCARDABLE:
            continue
        keep.append(s)

    # 按 RVA 排序
    keep.sort(key=lambda s: s["vaddr"])

    # 2) 计算 ELF 头部布局
    ehsize    = 64
    phentsize = 56
    phnum     = len(keep)
    phoff     = ehsize
    data_off  = phoff + phnum * phentsize
    data_off  = (data_off + FILE_ALIGN - 1) & ~(FILE_ALIGN - 1)

    # 3) 组装 payload 与 program headers
    payload = bytearray()
    phs = []

    print(f"[pe_to_elf] image_base=0x{pe['image_base']:x} "
          f"entry_rva=0x{pe['entry_rva']:x} "
          f"size_image=0x{pe['size_image']:x}",
          flush=True)

    for s in keep:
        rva     = s["vaddr"]
        vma     = vma_base + rva
        lma     = lma_base + rva
        filesz  = s["size_raw"]
        memsz   = s["vsize"] if s["vsize"] > 0 else filesz
        if memsz < filesz:
            memsz = filesz

        flags = 0
        if s["char"] & IMAGE_SCN_MEM_EXECUTE: flags |= 0x1   # PF_X
        if s["char"] & IMAGE_SCN_MEM_WRITE:   flags |= 0x2   # PF_W
        if s["char"] & IMAGE_SCN_MEM_READ:    flags |= 0x4   # PF_R
        # 若都没有，至少给 R
        if flags == 0:
            flags = 0x4

        cur_off = data_off + len(payload)
        phs.append({
            "type":   1,           # PT_LOAD
            "flags":  flags,
            "offset": cur_off,
            "vaddr":  vma,
            "paddr":  lma,
            "filesz": filesz,
            "memsz":  memsz,
            "align":  FILE_ALIGN,
        })

        # 文件里只写 filesz 字节；memsz 超出部分由 loader 补零
        if filesz > 0:
            payload += pe["data"][s["ptr_raw"]:s["ptr_raw"] + filesz]
        # 按 FILE_ALIGN 对齐到下一页
        pad = (-len(payload)) % FILE_ALIGN
        payload += b"\0" * pad

        print(f"[pe_to_elf] SECTION {s['name']:<8} "
              f"rva=0x{rva:08x} vma=0x{vma:016x} paddr=0x{lma:016x} "
              f"filesz=0x{filesz:x} memsz=0x{memsz:x} flags={flags}",
              flush=True)

    # 4) 计算入口
    entry_vma = vma_base + pe["entry_rva"]

    # 5) 构造 ELF header
    ehdr = bytearray(ehsize)
    ehdr[0:4] = b"\x7fELF"
    ehdr[4]   = 2       # ELFCLASS64
    ehdr[5]   = 1       # ELFDATA2LSB
    ehdr[6]   = 1       # EV_CURRENT
    ehdr[7]   = 0       # System V ABI
    struct.pack_into("<H", ehdr, 16, 2)          # e_type = ET_EXEC
    struct.pack_into("<H", ehdr, 18, 0x3E)       # e_machine = EM_X86_64
    struct.pack_into("<I", ehdr, 20, 1)          # e_version
    struct.pack_into("<Q", ehdr, 24, entry_vma)  # e_entry
    struct.pack_into("<Q", ehdr, 32, phoff)      # e_phoff
    struct.pack_into("<Q", ehdr, 40, 0)          # e_shoff
    struct.pack_into("<I", ehdr, 48, 0)          # e_flags
    struct.pack_into("<H", ehdr, 52, ehsize)
    struct.pack_into("<H", ehdr, 54, phentsize)
    struct.pack_into("<H", ehdr, 56, phnum)
    struct.pack_into("<H", ehdr, 58, 64)         # e_shentsize
    struct.pack_into("<H", ehdr, 60, 0)          # e_shnum
    struct.pack_into("<H", ehdr, 62, 0)          # e_shstrndx

    # 6) 构造 program headers
    phbuf = bytearray()
    for ph in phs:
        p = bytearray(phentsize)
        struct.pack_into("<I", p, 0,  ph["type"])
        struct.pack_into("<I", p, 4,  ph["flags"])
        struct.pack_into("<Q", p, 8,  ph["offset"])
        struct.pack_into("<Q", p, 16, ph["vaddr"])
        struct.pack_into("<Q", p, 24, ph["paddr"])
        struct.pack_into("<Q", p, 32, ph["filesz"])
        struct.pack_into("<Q", p, 40, ph["memsz"])
        struct.pack_into("<Q", p, 48, ph["align"])
        phbuf += p

    # 7) 写出文件
    with open(out_path, "wb") as f:
        f.write(ehdr)
        f.write(phbuf)
        # 填充到 data_off
        gap = data_off - (len(ehdr) + len(phbuf))
        if gap > 0:
            f.write(b"\0" * gap)
        f.write(payload)

    print(f"[pe_to_elf] wrote {out_path}: {phnum} PT_LOAD, "
          f"entry=0x{entry_vma:016x}, "
          f"total_file=0x{data_off + len(payload):x}",
          flush=True)


def main():
    if len(sys.argv) != 5:
        print("usage: pe_to_elf.py <input.pe> <output.elf> "
              "<lma_base> <vma_base>", file=sys.stderr)
        sys.exit(1)

    inp   = sys.argv[1]
    outp  = sys.argv[2]
    lma   = int(sys.argv[3], 0)
    vma   = int(sys.argv[4], 0)

    pe = read_pe(inp)
    write_elf(pe, outp, lma, vma)


if __name__ == "__main__":
    main()