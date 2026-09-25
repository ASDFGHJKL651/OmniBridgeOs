/*===OmniBridgeOs/kernel/arch/x64/uel.c===*/
#include "uel.h"
#include "obr.h"
#include "vfs.h"
#include "kmalloc.h"
#include "serial.h"
#include "audit.h"
#include "permission.h"

/* 第 14 步新增：签名验证 */
#include "ita_sign.h"
#include "ed25519.h"

/* ---------- 工具 ---------- */

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* ---------- 格式识别 ---------- */

enum uel_format uel_detect_format(const void *buf, uint64_t size)
{
    if (!buf || size < 4) return UEL_FMT_UNKNOWN;

    const uint8_t *p = (const uint8_t *)buf;

    /* 1) OBR：头 4 字节按 little-endian 读应为 OBR_MAGIC */
    if (read_le32(p) == OBR_MAGIC) return UEL_FMT_OBR;

    /* 2) PE：MZ */
    if (p[0] == 'M' && p[1] == 'Z') return UEL_FMT_PE;

    /* 3) ELF：0x7F 'E' 'L' 'F' */
    if (p[0] == 0x7F && p[1] == 'E' && p[2] == 'L' && p[3] == 'F')
        return UEL_FMT_ELF;

    return UEL_FMT_UNKNOWN;
}

/* ---------- 第 14 步：签名验证辅助 ---------- */

/*
 * 对 .obr 镜像执行签名验证。在复制任何 LOAD 段之前调用。
 *
 * 成功：返回 0，并在 out->flags 中设置相应位。
 * 失败：返回负错误码；调用方应立即中止加载（尚未复制段，无副作用）。
 *
 * 人工必须审查：
 *   - 签名位于文件末尾 64 字节；被签名内容为 [0, size-64)。
 *   - 该约定必须与 obcc 工具链、OB_InternalSign 严格一致。
 */
static int uel_verify_signature(const struct obr_header *h,
                                const uint8_t *buf, uint64_t size,
                                struct task_t *caller,
                                struct uel_load_result *out)
{
    /* 沙盒进程禁止加载任何签名文件（含 sig_type != 0） */
    int sandbox = (caller && caller->sandbox_flags != 0);

    if (h->sig_type == 0x00) {
        /* 未签名：允许加载，打印 WARN；不设置任何 VERIFIED / SIGNATURE flag */
        serial_printf("[UEL] WARN: .obr unsigned (sig_type=0)\n");
        if (sandbox) {
            serial_printf("[UEL] sandbox cannot load unsigned .obr\n");
            return OB_EPERM;
        }
        return 0;
    }

    if (h->sig_type == 0x01) {
        /* Ed25519 系统绑定签名 */
        if (h->sig_length != ED25519_SIG_LEN) {
            serial_printf("[UEL] sig_length=%u != %u\n",
                          (unsigned)h->sig_length,
                          (unsigned)ED25519_SIG_LEN);
            return OB_EIO;
        }
        if (size < (uint64_t)ED25519_SIG_LEN + sizeof(struct obr_header)) {
            serial_printf("[UEL] image too small to contain signature\n");
            return OB_EIO;
        }

        /* 沙盒永远不允许加载签名文件（§5.8.4） */
        if (sandbox) {
            serial_printf("[UEL] sandbox cannot load signed .obr\n");
            return OB_EPERM;
        }

        const uint8_t *sig = buf + size - ED25519_SIG_LEN;
        uint64_t msg_len   = size - ED25519_SIG_LEN;

        /* 公钥来源：优先内核当前的临时/持久密钥 */
        uint8_t pub[ED25519_PUBKEY_LEN];
        int have_pub = 0;

        if (ita_sign_ready()) {
            if (ita_sign_export_pubkey(pub) == 0) {
                have_pub = 1;
            }
        }

        /* 回退：从根文件系统的 OBFS 超级块读取。
         * 本步 root 是 tmpfs，无该字段；此路径预留。 */
        if (!have_pub) {
            /* 尝试从挂载点找 OBFS superblock —— 本步未实现，
             * 直接报告失败。 */
            serial_printf("[UEL] no pubkey available, signed .obr rejected\n");
            return OB_EACCES;
        }

        if (!ita_verify_signature(pub, buf, msg_len, sig)) {
            serial_printf("[UEL] signature verify FAILED\n");
            return OB_EACCES;
        }

        out->flags |= UEL_FLAG_OBR_SIGNATURE_OK;
        serial_printf("[UEL] signature OK (Ed25519)\n");
        return 0;
    }

    if (h->sig_type == 0x02) {
        /* 管理员手动标记：本步未实现 */
        serial_printf("[UEL] sig_type=0x02 (admin) not implemented\n");
        if (caller) {
            audit_critical_access(caller->pid,
                                  "(uel-admin-sig)",
                                  OB_ACCESS_READ);
        }
        return OB_ENOSYS;
    }

    serial_printf("[UEL] unknown sig_type=0x%x\n",
                  (unsigned)h->sig_type);
    return OB_EIO;
}

/* ---------- .obr 加载 ---------- */

static int uel_load_obr(const uint8_t *buf, uint64_t size,
                        uint64_t load_base,
                        struct task_t *caller,
                        struct uel_load_result *out)
{
    const struct obr_header *h = (const struct obr_header *)buf;

    int rc = obr_validate_header(h, size);
    if (rc != 0) {
        serial_printf("[UEL] load failed: invalid obr header rc=%d\n", rc);
        return rc;
    }

    if (h->arch != OBR_ARCH_X64) {
        serial_printf("[UEL] load failed: arch=%u (only x64)\n",
                      (unsigned)h->arch);
        return OB_EINVAL;
    }

    /* min_privilege 预检 */
    uint8_t  caller_priv = 9;
    uint64_t caller_pid  = 0;
    if (caller) {
        caller_priv = caller->privilege_level;
        caller_pid  = caller->pid;
    }
    if (caller_priv < h->min_privilege) {
        serial_printf("[UEL] load failed: min_privilege=%u caller_priv=%u\n",
                      (unsigned)h->min_privilege, (unsigned)caller_priv);
        audit_critical_access(caller_pid, "(uel-load)", OB_ACCESS_EXEC);
        return OB_EPERM;
    }

    /* ★ 第 14 步：签名验证（在复制任何 LOAD 段之前）。
     * 若验证失败，尚未写入 load_base，避免污染调用方缓冲区。 */
    rc = uel_verify_signature(h, buf, size, caller, out);
    if (rc != 0) {
        return rc;
    }

    /* 遍历 phdr：LOAD 复制，DYNAMIC/INTERP 仅标记 */
    uint64_t ph_size = (uint64_t)h->ph_count *
                       (uint64_t)sizeof(struct obr_phdr);
    if (h->ph_offset + ph_size > size) {
        serial_printf("[UEL] load failed: phdr table out of range\n");
        return OB_EIO;
    }

    for (uint16_t i = 0; i < h->ph_count; ++i) {
        const struct obr_phdr *ph = (const struct obr_phdr *)
            (buf + h->ph_offset + (uint64_t)i * sizeof(struct obr_phdr));

        if (ph->type == OBR_PT_DYNAMIC) {
            out->flags |= UEL_FLAG_OBR_HAS_DYNAMIC;
            continue;
        }
        if (ph->type == OBR_PT_INTERP) {
            out->flags |= UEL_FLAG_OBR_HAS_INTERP;
            continue;
        }
        if (ph->type != OBR_PT_LOAD) continue;

        if (ph->filesz > ph->memsz) {
            serial_printf("[UEL] load failed: filesz > memsz\n");
            return OB_EIO;
        }
        if (ph->offset + ph->filesz > size) {
            serial_printf("[UEL] load failed: segment out of image\n");
            return OB_EIO;
        }

        uint64_t dst     = load_base + ph->vaddr;
        uint64_t dst_end = dst + ph->memsz;
        if (dst_end < dst) {  /* 回绕 = 溢出 */
            serial_printf("[UEL] load failed: address overflow\n");
            return OB_EINVAL;
        }

        uint8_t       *d = (uint8_t *)(uintptr_t)dst;
        const uint8_t *s = buf + ph->offset;

        for (uint64_t k = 0; k < ph->filesz; ++k) d[k] = s[k];
        for (uint64_t k = ph->filesz; k < ph->memsz; ++k) d[k] = 0;
    }

    out->entry  = load_base + h->entry_point;

    if (out->flags & UEL_FLAG_OBR_HAS_DYNAMIC) {
        serial_printf("[UEL] WARN: PT_DYNAMIC present, deps not loaded\n");
    }
    if (out->flags & UEL_FLAG_OBR_HAS_INTERP) {
        serial_printf("[UEL] WARN: PT_INTERP present, interp not loaded\n");
    }

    serial_printf("[UEL] format=obr entry=0x%llx load_base=0x%llx "
                  "size=%llu flags=0x%x\n",
                  (unsigned long long)out->entry,
                  (unsigned long long)out->load_base,
                  (unsigned long long)out->total_size,
                  (unsigned)out->flags);
    return 0;
}

/* ---------- 主入口 ---------- */

int uel_load(const void *buf, uint64_t size,
             uint64_t load_base,
             struct task_t *caller,
             struct uel_load_result *out)
{
    if (!buf || !out) return OB_EINVAL;

    if (size > UEL_MAX_IMAGE_SIZE) {
        serial_printf("[UEL] image too large: %llu > %llu\n",
                      (unsigned long long)size,
                      (unsigned long long)UEL_MAX_IMAGE_SIZE);
        return OB_EINVAL;
    }

    out->format     = UEL_FMT_UNKNOWN;
    out->entry      = 0;
    out->load_base  = load_base;
    out->total_size = size;
    out->flags      = 0;

    enum uel_format fmt = uel_detect_format(buf, size);
    out->format = fmt;

    switch (fmt) {
    case UEL_FMT_OBR:
        return uel_load_obr((const uint8_t *)buf, size,
                            load_base, caller, out);

    case UEL_FMT_PE:
        serial_printf("[UEL] format=pe (not implemented in step 14)\n");
        return OB_ENOSYS;

    case UEL_FMT_ELF:
        serial_printf("[UEL] format=elf (not implemented in step 14)\n");
        return OB_ENOSYS;

    default:
        serial_printf("[UEL] load failed: unknown format\n");
        return OB_EINVAL;
    }
}

/* ---------- VFS 便捷入口 ---------- */

int uel_load_path(const char *path,
                  uint64_t load_base,
                  struct task_t *caller,
                  struct uel_load_result *out)
{
    if (!path || !out) return OB_EINVAL;

    struct vfs_file *f = 0;
    int rc = vfs_open(path, VFS_O_RDONLY, &f);
    if (rc != 0) return rc;

    /* 增量读取到动态缓冲区（上限 UEL_MAX_IMAGE_SIZE） */
    uint64_t cap   = 4096;
    uint64_t total = 0;
    uint8_t *buf = (uint8_t *)kmalloc((size_t)cap);
    if (!buf) {
        vfs_close(f);
        return OB_ENOMEM;
    }

    for (;;) {
        if (total >= UEL_MAX_IMAGE_SIZE) {
            kfree(buf);
            vfs_close(f);
            serial_printf("[UEL] image too large (>%llu)\n",
                          (unsigned long long)UEL_MAX_IMAGE_SIZE);
            return OB_EINVAL;
        }
        if (total >= cap) {
            uint64_t ncap = cap * 2;
            if (ncap > UEL_MAX_IMAGE_SIZE) ncap = UEL_MAX_IMAGE_SIZE;
            uint8_t *nb = (uint8_t *)kmalloc((size_t)ncap);
            if (!nb) {
                kfree(buf);
                vfs_close(f);
                return OB_ENOMEM;
            }
            for (uint64_t i = 0; i < total; ++i) nb[i] = buf[i];
            kfree(buf);
            buf = nb;
            cap = ncap;
        }

        int64_t n = vfs_read(f, buf + total, cap - total);
        if (n < 0) {
            kfree(buf);
            vfs_close(f);
            return (int)n;
        }
        if (n == 0) break;
        total += (uint64_t)n;
    }
    vfs_close(f);

    rc = uel_load(buf, total, load_base, caller, out);
    kfree(buf);
    return rc;
}
/*===OmniBridgeOs/kernel/arch/x64/uel.c 结束===*/