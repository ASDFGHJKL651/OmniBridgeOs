/*===OmniBridgeOs/kernel/arch/x64/net/pci.h===*/
#ifndef OMNIBRIDGE_NET_PCI_H
#define OMNIBRIDGE_NET_PCI_H

#include <stdint.h>

#define PCI_VENDOR_VIRTIO 0x1AF4u
#define PCI_DEV_VIRTIO_NET_LEGACY 0x1000u
#define PCI_DEV_VIRTIO_NET_MODERN 0x1041u

struct pci_device {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor;
    uint16_t device;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  irq_line;
    uint8_t  _pad;
    uint64_t bar[6];
    uint8_t  bar_is_io[6];
};

void pci_init(void);

int  pci_find_device(uint16_t vendor, uint16_t device,
                     struct pci_device *out);
int  pci_find_class(uint8_t cls, uint8_t sub, struct pci_device *out);

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint8_t  pci_config_read8 (uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void     pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func,
                            uint8_t off, uint32_t val);
void     pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func,
                            uint8_t off, uint16_t val);

int  pci_enable_bus_master(const struct pci_device *dev);
int  pci_read_bar(const struct pci_device *dev, int bar, uint64_t *out_base,
                  uint64_t *out_size, uint8_t *out_is_io);

/* ★ 映射 BAR：若为 MMIO，返回内核可直接访问的虚拟地址。
 *   当前低恒等映射覆盖 0..4GB，因此直接返回物理基址。
 *   若后续恒等映射取消，应改为 DIRECTMAP_BASE + phys。 */
int  pci_map_bar(struct pci_device *dev, int bar,
                 uint64_t *out_vaddr, uint64_t *out_size);

/* ★ MSI 接口占位（本步不实现完整 MSI，仅保留接口） */
int  pci_enable_msi(struct pci_device *dev, uint8_t vector);
int  pci_disable_msi(struct pci_device *dev);

#endif /* OMNIBRIDGE_NET_PCI_H */
/*===OmniBridgeOs/kernel/arch/x64/net/pci.h 结束===*/