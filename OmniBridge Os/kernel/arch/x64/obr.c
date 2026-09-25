/*===OmniBridgeOs/kernel/arch/x64/obr.c===*/
#include "obr.h"
#include "serial.h"
#include "task.h"   /* OB_EIO / OB_ENOSYS */

int obr_validate_header(const struct obr_header *h, uint64_t image_size)
{
    if (!h) return OB_EIO;
    if (image_size < sizeof(struct obr_header)) return OB_EIO;
    if (h->magic != OBR_MAGIC)   return OB_EIO;
    if (h->version != OBR_VERSION) return OB_EIO;
    if (h->arch != OBR_ARCH_X64) return OB_EIO;

    /* phdr 表边界（用 uint64_t 计算防止溢出） */
    uint64_t ph_size = (uint64_t)h->ph_count *
                       (uint64_t)sizeof(struct obr_phdr);
    if (h->ph_offset + ph_size > image_size) {
        return OB_EIO;
    }

    return 0;
}

int obr_load(const void *image, uint64_t image_size,
             uint64_t load_base,
             void **out_entry)
{
    if (!image || !out_entry) return OB_EIO;

    const uint8_t *base = (const uint8_t *)image;
    const struct obr_header *h = (const struct obr_header *)image;

    int rc = obr_validate_header(h, image_size);
    if (rc != 0) {
        serial_printf("[OBR] header validation failed rc=%d\n", rc);
        return rc;
    }

    if (h->ph_count == 0) {
        serial_printf("[OBR] warning: ph_count=0, entry direct\n");
    }

    for (uint16_t i = 0; i < h->ph_count; ++i) {
        const struct obr_phdr *ph =
            (const struct obr_phdr *)(base + h->ph_offset +
                (uint64_t)i * sizeof(struct obr_phdr));

        if (ph->type != OBR_PT_LOAD) {
            serial_printf("[OBR] warning: skip phdr type=%u (i=%u)\n",
                          (unsigned)ph->type, (unsigned)i);
            continue;
        }

        /* 边界检查 */
        if (ph->offset + ph->filesz > image_size) {
            serial_printf("[OBR] LOAD segment out of file bounds\n");
            return OB_EIO;
        }
        if (ph->filesz > ph->memsz) {
            serial_printf("[OBR] LOAD filesz > memsz\n");
            return OB_EIO;
        }

        uint64_t dst = load_base + ph->vaddr;
        uint8_t *d = (uint8_t *)(uintptr_t)dst;
        const uint8_t *s = base + ph->offset;

        /* 按字节拷贝，避免未对齐访问 */
        for (uint64_t k = 0; k < ph->filesz; ++k) d[k] = s[k];
        /* BSS 清零 */
        for (uint64_t k = ph->filesz; k < ph->memsz; ++k) d[k] = 0;
    }

    *out_entry = (void *)(uintptr_t)(load_base + h->entry_point);
    return 0;
}
/*===OmniBridgeOs/kernel/arch/x64/obr.c 结束===*/