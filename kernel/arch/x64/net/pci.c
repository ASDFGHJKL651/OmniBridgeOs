/*===OmniBridgeOs/kernel/arch/x64/net/pci.c===*/
#include "pci.h"
#include "serial.h"
#include "vmm.h"                    /* ★ 第 18C 步：DIRECTMAP_BASE */
#include <vfs.h>

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static inline void outl_(uint16_t p, uint32_t v)
{ __asm__ __volatile__("outl %0, %1" :: "a"(v), "Nd"(p)); }
static inline uint32_t inl_(uint16_t p)
{ uint32_t v; __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)slot << 11)
                  | ((uint32_t)func << 8)
                  | (off & 0xFCu);
    outl_(PCI_CONFIG_ADDR, addr);
    return inl_(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    uint32_t v = pci_config_read32(bus, slot, func, off & 0xFCu);
    return (uint16_t)(v >> ((off & 2) * 8));
}

uint8_t pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    uint32_t v = pci_config_read32(bus, slot, func, off & 0xFCu);
    return (uint8_t)(v >> ((off & 3) * 8));
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func,
                        uint8_t off, uint32_t val)
{
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)slot << 11)
                  | ((uint32_t)func << 8)
                  | (off & 0xFCu);
    outl_(PCI_CONFIG_ADDR, addr);
    __asm__ __volatile__("outl %0, %1" :: "a"(val), "Nd"((uint16_t)PCI_CONFIG_DATA));
}

void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func,
                        uint8_t off, uint16_t val)
{
    uint32_t cur = pci_config_read32(bus, slot, func, off & 0xFCu);
    uint32_t shift = (off & 2) * 8;
    cur = (cur & ~(0xFFFFu << shift)) | ((uint32_t)val << shift);
    pci_config_write32(bus, slot, func, off & 0xFCu, cur);
}

void pci_init(void)
{
    serial_printf("[PCI] init: config ports 0xCF8/0xCFC\n");
}

int pci_find_device(uint16_t vendor, uint16_t device, struct pci_device *out)
{
    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            for (uint8_t func = 0; func < 8; ++func) {
                uint32_t id = pci_config_read32((uint8_t)bus, slot, func, 0x00);
                uint16_t v = (uint16_t)(id & 0xFFFF);
                if (v == 0xFFFF) {
                    if (func == 0) break;
                    continue;
                }
                uint16_t d = (uint16_t)(id >> 16);
                if (v == vendor && (device == 0xFFFF || d == device)) {
                    if (!out) return 0;
                    out->bus  = (uint8_t)bus;
                    out->slot = slot;
                    out->func = func;
                    out->vendor = v;
                    out->device = d;
                    uint32_t cls = pci_config_read32((uint8_t)bus, slot, func, 0x08);
                    out->revision  = (uint8_t)(cls & 0xFF);
                    out->prog_if   = (uint8_t)((cls >> 8) & 0xFF);
                    out->subclass  = (uint8_t)((cls >> 16) & 0xFF);
                    out->class_code= (uint8_t)((cls >> 24) & 0xFF);
                    out->irq_line  = pci_config_read8((uint8_t)bus, slot, func, 0x3C);
                    for (int b = 0; b < 6; ++b) {
                        uint32_t barv = pci_config_read32((uint8_t)bus, slot, func,
                                                          (uint8_t)(0x10 + 4 * b));
                        out->bar[b]       = barv;
                        out->bar_is_io[b] = (uint8_t)(barv & 1u);
                    }
                    return 0;
                }
                if (func == 0) {
                    uint8_t hdr = pci_config_read8((uint8_t)bus, slot, 0, 0x0E);
                    if (!(hdr & 0x80)) break;
                }
            }
        }
    }
    return -1;
}

int pci_find_class(uint8_t cls, uint8_t sub, struct pci_device *out)
{
    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            for (uint8_t func = 0; func < 8; ++func) {
                uint32_t id = pci_config_read32((uint8_t)bus, slot, func, 0);
                if ((uint16_t)(id & 0xFFFF) == 0xFFFF) {
                    if (func == 0) break;
                    continue;
                }
                uint32_t c = pci_config_read32((uint8_t)bus, slot, func, 0x08);
                if ((uint8_t)(c >> 24) == cls && (uint8_t)(c >> 16) == sub) {
                    if (out) {
                        out->bus = (uint8_t)bus; out->slot = slot; out->func = func;
                        out->vendor = (uint16_t)(id & 0xFFFF);
                        out->device = (uint16_t)(id >> 16);
                        out->class_code = cls;
                        out->subclass   = sub;
                        out->prog_if    = (uint8_t)(c >> 8);
                        out->revision   = (uint8_t)c;
                    }
                    return 0;
                }
                if (func == 0) {
                    uint8_t hdr = pci_config_read8((uint8_t)bus, slot, 0, 0x0E);
                    if (!(hdr & 0x80)) break;
                }
            }
        }
    }
    return -1;
}

int pci_enable_bus_master(const struct pci_device *dev)
{
    if (!dev) return -1;
    uint16_t cmd = pci_config_read16(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= 0x0001;  /* I/O Space Enable */
    cmd |= 0x0002;  /* Memory Space Enable */
    cmd |= 0x0004;  /* Bus Master Enable */
    pci_config_write16(dev->bus, dev->slot, dev->func, 0x04, cmd);
    serial_printf("[PCI] %02x:%02x.%u cmd=0x%04x\n",
                  dev->bus, dev->slot, dev->func, (unsigned)cmd);
    return 0;
}

int pci_read_bar(const struct pci_device *dev, int bar,
                 uint64_t *out_base, uint64_t *out_size, uint8_t *out_is_io)
{
    if (!dev || bar < 0 || bar > 5) return -1;
    uint8_t off = (uint8_t)(0x10 + 4 * bar);

    uint32_t orig = pci_config_read32(dev->bus, dev->slot, dev->func, off);
    if (orig == 0) return -1;

    pci_config_write32(dev->bus, dev->slot, dev->func, off, 0xFFFFFFFFu);
    uint32_t mask = pci_config_read32(dev->bus, dev->slot, dev->func, off);
    pci_config_write32(dev->bus, dev->slot, dev->func, off, orig);

    if (mask == 0) return -1;

    uint8_t is_io = (uint8_t)(orig & 1u);
    if (is_io) {
        uint32_t base = orig & 0xFFFFFFFCu;
        uint32_t m    = mask & 0xFFFFFFFCu;
        if (out_base) *out_base = base;
        if (out_size) *out_size = (~m + 1) & 0xFFFFu;
        if (out_is_io) *out_is_io = 1;
    } else {
        uint32_t base = orig & 0xFFFFFFF0u;
        uint32_t m    = mask & 0xFFFFFFF0u;
        if (out_base) *out_base = base;
        if (out_size) *out_size = (~m + 1) & 0xFFFFFFFFu;
        if (out_is_io) *out_is_io = 0;
    }
    return 0;
}

/*
 * ★ 第 18C 步关键修改：
 *   pci_map_bar 现在返回 **DirectMap 虚拟地址**，而不是物理地址。
 *
 *   原因：
 *     vmm_create_address_space 不再复制 PML4[0]（内核低恒等映射）。
 *     用户 PML4 切换后，物理地址 base 不能直接解引用；必须通过
 *     DirectMap（PML4[256]）访问。DirectMap 在所有 PML4 中被复制，
 *     因此无论当前 CR3 是内核主 PML4 还是用户 PML4，返回的虚拟地址
 *     都是有效的。
 *
 *   调用方（virtio_net.c / virtio_blk.c）原来就期望拿到可以直接读写
 *   的虚拟地址；本修改只是保证这些虚拟地址在切换 CR3 后仍有效。
 */
int pci_map_bar(struct pci_device *dev, int bar,
                uint64_t *out_vaddr, uint64_t *out_size)
{
    if (!dev || !out_vaddr || !out_size) return -1;
    uint64_t base = 0, size = 0;
    uint8_t is_io = 0;
    if (pci_read_bar(dev, bar, &base, &size, &is_io) != 0) return -1;
    if (is_io) {
        serial_printf("[PCI] BAR%d is I/O, not MMIO\n", bar);
        return -1;
    }
    *out_vaddr = DIRECTMAP_BASE + base;
    *out_size  = size;
    serial_printf("[PCI] map BAR%d base=0x%llx vaddr=0x%llx size=0x%llx\n",
                  bar,
                  (unsigned long long)base,
                  (unsigned long long)(*out_vaddr),
                  (unsigned long long)size);
    return 0;
}

int pci_enable_msi(struct pci_device *dev, uint8_t vector)
{
    (void)dev; (void)vector;
    serial_printf("[PCI] WARN: MSI not implemented in 18A (stub)\n");
    return OB_ENOSYS;
}

int pci_disable_msi(struct pci_device *dev)
{
    (void)dev;
    return 0;
}
/*===OmniBridgeOs/kernel/arch/x64/net/pci.c 结束===*/