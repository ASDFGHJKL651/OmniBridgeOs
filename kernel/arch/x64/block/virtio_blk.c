/*===OmniBridgeOs/kernel/arch/x64/block/virtio_blk.c===*/
/*
 * VirtIO 块设备驱动（第 18B 步）。
 *
 * 复用第 18A 步的 pci_* 接口（kernel/arch/x64/net/pci.c）。
 * 使用 legacy virtio PCI 接口（device id 0x1001）。
 *
 * 请求格式（VirtIO 1.0 §5.2.6）：
 *   struct virtio_blk_req {
 *       uint32_t type;      // 0=IN, 1=OUT, 4=FLUSH
 *       uint32_t reserved;
 *       uint64_t sector;    // 起始扇区（512 字节/扇区）
 *       uint8_t  data[];    // 数据（IN 时由设备填充，OUT 时由驱动填充）
 *       uint8_t  status;    // 0=OK, 1=IOERR, 2=UNSUPP
 *   };
 *
 * 由于未协商 VIRTIO_BLK_F_FLUSH，FLUSH 请求不发送；依赖同步语义。
 */
#include "virtio_blk.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "serial.h"
#include "spinlock.h"

/* ---------- virtio PCI legacy 寄存器偏移 ---------- */
#define VIRTIO_PCI_HOST_FEATURES   0x00
#define VIRTIO_PCI_GUEST_FEATURES  0x04
#define VIRTIO_PCI_QUEUE_PFN       0x08
#define VIRTIO_PCI_QUEUE_NUM       0x0C
#define VIRTIO_PCI_QUEUE_SEL       0x0E
#define VIRTIO_PCI_QUEUE_NOTIFY    0x10
#define VIRTIO_PCI_STATUS          0x12
#define VIRTIO_PCI_ISR             0x13
#define VIRTIO_PCI_CONFIG          0x14

#define VIRTIO_STATUS_ACK       1
#define VIRTIO_STATUS_DRIVER    2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FAILED    128

#define VQ_SIZE     16
#define VQ_MAX_NUM  256

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VQ_MAX_NUM];
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[VQ_MAX_NUM];
} __attribute__((packed));

struct vq {
    struct vring_desc  *desc;
    struct vring_avail *avail;
    struct vring_used  *used;
    uint16_t num;
    uint16_t use_num;
    uint16_t last_used;
    uint16_t free_head;
    uint16_t num_free;
    uint64_t vring_pa;
    uint8_t *bufs;
    uint64_t phys_bufs;
    int      ready;
};

/* 每个描述符缓冲区大小：header(16) + data(512) + status(1) 向上取整 */
#define VBLK_BUF_SIZE   1024
#define VBLK_HDR_SIZE   16

struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

#define VIRTIO_BLK_T_IN      0
#define VIRTIO_BLK_T_OUT     1
#define VIRTIO_BLK_T_FLUSH   4

#define VIRTIO_BLK_S_OK      0
#define VIRTIO_BLK_S_IOERR   1
#define VIRTIO_BLK_S_UNSUPP  2

struct vblk_dev {
    uint16_t io_base;
    uint8_t  mac_pad[6];
    uint64_t capacity_sectors;   /* 从 config 读取 */
    struct vq q;
    struct block_device bdev;
    int ready;
    spinlock_t lock;
};

static struct vblk_dev g_vblk;

/* ---------- 端口 I/O ---------- */
static inline void outb_(uint16_t p, uint8_t v)
{ __asm__ __volatile__("outb %0, %1" :: "a"(v), "Nd"(p)); }
static inline uint8_t inb_(uint16_t p)
{ uint8_t v; __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outw_(uint16_t p, uint16_t v)
{ __asm__ __volatile__("outw %0, %1" :: "a"(v), "Nd"(p)); }
static inline uint16_t inw_(uint16_t p)
{ uint16_t v; __asm__ __volatile__("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outl_(uint16_t p, uint32_t v)
{ __asm__ __volatile__("outl %0, %1" :: "a"(v), "Nd"(p)); }
static inline uint32_t inl_(uint16_t p)
{ uint32_t v; __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }

#define VIRTIO_WMB()  __asm__ __volatile__("sfence" ::: "memory")
#define VIRTIO_MB()   __asm__ __volatile__("mfence" ::: "memory")

static inline uint16_t used_idx_load(const struct vq *q)
{ return *(volatile const uint16_t *)(const void *)&q->used->idx; }
static inline uint32_t used_id_load(const struct vq *q, uint16_t i)
{ return *(volatile const uint32_t *)(const void *)&q->used->ring[i].id; }
static inline uint16_t avail_idx_load(const struct vq *q)
{ return *(volatile const uint16_t *)(const void *)&q->avail->idx; }
static inline void avail_idx_store(struct vq *q, uint16_t v)
{ *(volatile uint16_t *)(void *)&q->avail->idx = v; }
static inline void avail_ring_store(struct vq *q, uint16_t i, uint16_t v)
{ *(volatile uint16_t *)(void *)&q->avail->ring[i] = v; }

/* ============================================================
 * 队列初始化
 * ============================================================ */
static int vq_init(struct vq *q, uint16_t io_base, uint8_t *buf_mem,
                   uint64_t buf_phys)
{
    outw_(io_base + VIRTIO_PCI_QUEUE_SEL, 0);
    uint16_t full_num = inw_(io_base + VIRTIO_PCI_QUEUE_NUM);
    if (full_num == 0) {
        serial_printf("[VIRTIO-BLK] queue_num=0\n");
        return -1;
    }
    if (full_num > VQ_MAX_NUM) full_num = VQ_MAX_NUM;

    uint16_t use_num = (full_num < VQ_SIZE) ? full_num : VQ_SIZE;

    q->num       = full_num;
    q->use_num   = use_num;
    q->last_used = 0;
    q->free_head = 0;
    q->ready     = 0;

    uint32_t desc_off   = 0;
    uint32_t desc_size  = 16u * full_num;
    uint32_t avail_off  = desc_size;
    if (avail_off & 1u) avail_off++;
    uint32_t avail_size = 4u + 2u * full_num;
    uint32_t avail_end  = avail_off + avail_size;
    uint32_t used_off   = (avail_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t used_size  = 4u + 8u * full_num;
    uint32_t total      = used_off + used_size;

    int order = 0;
    while (((uint64_t)PAGE_SIZE << order) < total) order++;
    if (order > 8) return -1;

    struct page *pg = pmm_alloc_pages(order);
    if (!pg) return -1;

    uint64_t pa = page_to_phys(pg);
    uint8_t *va = (uint8_t *)(DIRECTMAP_BASE + pa);
    uint64_t alloc_bytes = (uint64_t)PAGE_SIZE << order;
    for (uint64_t i = 0; i < alloc_bytes; ++i) va[i] = 0;

    q->vring_pa = pa;
    q->desc     = (struct vring_desc  *)(va + desc_off);
    q->avail    = (struct vring_avail *)(va + avail_off);
    q->used     = (struct vring_used  *)(va + used_off);
    q->bufs     = buf_mem;
    q->phys_bufs = buf_phys;

    for (uint16_t i = 0; i < use_num; ++i) {
        q->desc[i].addr  = buf_phys + (uint64_t)i * VBLK_BUF_SIZE;
        q->desc[i].len   = VBLK_BUF_SIZE;
        q->desc[i].flags = 0;
        q->desc[i].next  = (uint16_t)((i + 1) % use_num);
    }
    if (use_num > 0) q->desc[use_num - 1].next = 0;

    q->avail->flags = 0;
    avail_idx_store(q, 0);
    q->num_free = use_num;

    uint32_t pfn = (uint32_t)(pa >> 12);
    outl_(io_base + VIRTIO_PCI_QUEUE_PFN, pfn);
    VIRTIO_MB();

    q->ready = 1;
    serial_printf("[VIRTIO-BLK] queue ready: dev_num=%u use=%u vring=0x%llx "
                  "buf=%llu\n",
                  (unsigned)full_num, (unsigned)use_num,
                  (unsigned long long)pa,
                  (unsigned long long)VBLK_BUF_SIZE);
    return 0;
}

/* ============================================================
 * 同步请求提交
 *
 * 人工必须审查：
 *   - 描述符链：hdr -> data -> status
 *   - 每次提交都等待设备处理完毕（轮询 used ring）。
 *   - 一次只允许一个请求（单请求串行化，由 dev->lock 保证）。
 * ============================================================ */
static int vblk_submit(struct vblk_dev *d, uint32_t type, uint64_t sector,
                       void *data, uint32_t data_len)
{
    struct vq *q = &d->q;
    if (!q->ready) return OB_EIO;
    if (q->num_free < 3) return OB_EAGAIN;

    /* 分配 3 个描述符（当前实现简单起见，假设连续） */
    uint16_t d0 = q->free_head;
    uint16_t d1 = q->desc[d0].next;
    uint16_t d2 = q->desc[d1].next;

    q->free_head = q->desc[d2].next;
    q->num_free -= 3;

    uint8_t *buf0 = q->bufs + (uint64_t)d0 * VBLK_BUF_SIZE;
    uint8_t *buf1 = q->bufs + (uint64_t)d1 * VBLK_BUF_SIZE;
    uint8_t *buf2 = q->bufs + (uint64_t)d2 * VBLK_BUF_SIZE;

    struct virtio_blk_req *hdr = (struct virtio_blk_req *)buf0;
    hdr->type     = type;
    hdr->reserved = 0;
    hdr->sector   = sector;

    if (type == VIRTIO_BLK_T_OUT && data && data_len > 0) {
        uint32_t copy = data_len < VBLK_BUF_SIZE ? data_len : VBLK_BUF_SIZE;
        const uint8_t *s = (const uint8_t *)data;
        for (uint32_t i = 0; i < copy; ++i) buf1[i] = s[i];
    } else if (type == VIRTIO_BLK_T_IN) {
        for (uint32_t i = 0; i < VBLK_BUF_SIZE; ++i) buf1[i] = 0;
    }

    buf2[0] = 0xFF;   /* status 未写入哨兵 */

    /* 描述符链 */
    q->desc[d0].addr  = q->phys_bufs + (uint64_t)d0 * VBLK_BUF_SIZE;
    q->desc[d0].len   = VBLK_HDR_SIZE;
    q->desc[d0].flags = 1;    /* NEXT */
    q->desc[d0].next  = d1;

    q->desc[d1].addr  = q->phys_bufs + (uint64_t)d1 * VBLK_BUF_SIZE;
    q->desc[d1].len   = data_len ? data_len : 1;
    q->desc[d1].flags = 1 | (type == VIRTIO_BLK_T_IN ? 2 : 0); /* NEXT | WRITE */
    q->desc[d1].next  = d2;

    q->desc[d2].addr  = q->phys_bufs + (uint64_t)d2 * VBLK_BUF_SIZE;
    q->desc[d2].len   = 1;
    q->desc[d2].flags = 2;    /* WRITE */
    q->desc[d2].next  = 0;

    /* 加入 avail ring */
    VIRTIO_WMB();
    uint16_t aidx = avail_idx_load(q);
    avail_ring_store(q, aidx % q->num, d0);
    VIRTIO_WMB();
    avail_idx_store(q, (uint16_t)(aidx + 1));

    /* notify */
    VIRTIO_MB();
    outw_(d->io_base + VIRTIO_PCI_QUEUE_NOTIFY, 0);

    /* 等待设备处理完毕（轮询） */
    for (int spin = 0; spin < 1000000; ++spin) {
        VIRTIO_MB();
        if (used_idx_load(q) != q->last_used) break;
        __asm__ __volatile__("pause" ::: "memory");
    }

    VIRTIO_MB();
    if (used_idx_load(q) == q->last_used) {
        serial_printf("[VIRTIO-BLK] request timeout\n");
        return OB_EIO;
    }

    uint16_t uidx = q->last_used % q->num;
    uint32_t did  = used_id_load(q, uidx);
    q->last_used++;

    /* 回收描述符链 */
    uint16_t cur = (uint16_t)did;
    for (int i = 0; i < 3; ++i) {
        uint16_t nxt = q->desc[cur].next;
        q->desc[cur].next = q->free_head;
        q->free_head = cur;
        q->num_free++;
        cur = nxt;
        if (cur == 0 && i < 2) break;
    }

    /* 检查 status */
    uint8_t st = buf2[0];
    if (st != VIRTIO_BLK_S_OK) {
        serial_printf("[VIRTIO-BLK] status=%u (sector=%llu type=%u)\n",
                      (unsigned)st,
                      (unsigned long long)sector, (unsigned)type);
        return OB_EIO;
    }

    if (type == VIRTIO_BLK_T_IN && data && data_len > 0) {
        uint32_t copy = data_len < VBLK_BUF_SIZE ? data_len : VBLK_BUF_SIZE;
        uint8_t *dst = (uint8_t *)data;
        for (uint32_t i = 0; i < copy; ++i) dst[i] = buf1[i];
    }

    return 0;
}

/* ============================================================
 * 块设备接口
 * ============================================================ */
static int vblk_read_sectors(struct block_device *dev, uint64_t lba,
                             uint32_t count, void *buf)
{
    struct vblk_dev *d = (struct vblk_dev *)dev->driver_data;
    if (!d || !d->ready) return OB_EIO;

    uint8_t *p = (uint8_t *)buf;
    for (uint32_t i = 0; i < count; ++i) {
        int rc = vblk_submit(d, VIRTIO_BLK_T_IN, lba + i,
                             p + (uint64_t)i * BLOCK_SECTOR_SIZE,
                             BLOCK_SECTOR_SIZE);
        if (rc != 0) return rc;
    }
    return 0;
}

static int vblk_write_sectors(struct block_device *dev, uint64_t lba,
                              uint32_t count, const void *buf)
{
    struct vblk_dev *d = (struct vblk_dev *)dev->driver_data;
    if (!d || !d->ready) return OB_EIO;

    const uint8_t *p = (const uint8_t *)buf;
    for (uint32_t i = 0; i < count; ++i) {
        int rc = vblk_submit(d, VIRTIO_BLK_T_OUT, lba + i,
                             (void *)(p + (uint64_t)i * BLOCK_SECTOR_SIZE),
                             BLOCK_SECTOR_SIZE);
        if (rc != 0) return rc;
    }
    return 0;
}

/* ============================================================
 * 初始化
 * ============================================================ */
int virtio_blk_init(void)
{
    struct pci_device dev;
    if (pci_find_device(PCI_VENDOR_VIRTIO, 0x1001, &dev) != 0) {
        if (pci_find_device(PCI_VENDOR_VIRTIO, 0x1042, &dev) != 0) {
            serial_printf("[VIRTIO-BLK] no VirtIO block device found\n");
            return -1;
        }
    }

    serial_printf("[VIRTIO-BLK] found %04x:%04x bus=%u slot=%u func=%u\n",
                  (unsigned)dev.vendor, (unsigned)dev.device,
                  (unsigned)dev.bus, (unsigned)dev.slot, (unsigned)dev.func);

    uint64_t base = 0, size = 0;
    uint8_t is_io = 0;
    if (pci_read_bar(&dev, 0, &base, &size, &is_io) != 0 || !is_io) {
        serial_printf("[VIRTIO-BLK] BAR0 not I/O\n");
        return -1;
    }
    pci_enable_bus_master(&dev);

    struct vblk_dev *d = &g_vblk;
    uint8_t *p = (uint8_t *)d;
    for (unsigned i = 0; i < sizeof(*d); ++i) p[i] = 0;
    d->io_base = (uint16_t)base;
    spin_lock_init(&d->lock);

    /* 复位 + 特性协商 */
    outb_(d->io_base + VIRTIO_PCI_STATUS, 0);
    VIRTIO_MB();
    outb_(d->io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACK);
    outb_(d->io_base + VIRTIO_PCI_STATUS,
          VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    uint32_t host_feat = inl_(d->io_base + VIRTIO_PCI_HOST_FEATURES);
    serial_printf("[VIRTIO-BLK] host features=0x%08x\n", (unsigned)host_feat);

    /* 不协商任何特性（保持最简行为） */
    outl_(d->io_base + VIRTIO_PCI_GUEST_FEATURES, 0);
    VIRTIO_MB();

    uint8_t st = inb_(d->io_base + VIRTIO_PCI_STATUS);
    if (st & VIRTIO_STATUS_FAILED) {
        serial_printf("[VIRTIO-BLK] device FAILED\n");
        return -1;
    }

    /* 读取容量（legacy virtio-blk config 偏移 0 处为 8 字节容量） */
    uint32_t cap_lo = inl_(d->io_base + VIRTIO_PCI_CONFIG + 0);
    uint32_t cap_hi = inl_(d->io_base + VIRTIO_PCI_CONFIG + 4);
    d->capacity_sectors = ((uint64_t)cap_hi << 32) | (uint64_t)cap_lo;
    if (d->capacity_sectors == 0) {
        serial_printf("[VIRTIO-BLK] capacity=0, abort\n");
        return -1;
    }
    serial_printf("[VIRTIO-BLK] capacity=%llu sectors (%llu MB)\n",
                  (unsigned long long)d->capacity_sectors,
                  (unsigned long long)(d->capacity_sectors / 2048));

    /* 分配队列缓冲区 */
    uint32_t buf_bytes = VQ_SIZE * VBLK_BUF_SIZE;
    uint64_t buf_pages = (buf_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    int buf_order = 0;
    while (((uint64_t)1 << buf_order) < buf_pages) buf_order++;
    struct page *bpg = pmm_alloc_pages(buf_order);
    if (!bpg) {
        serial_printf("[VIRTIO-BLK] DMA alloc failed\n");
        return -1;
    }
    uint64_t buf_phys = page_to_phys(bpg);
    uint8_t *buf_va = (uint8_t *)(DIRECTMAP_BASE + buf_phys);
    for (uint64_t i = 0; i < ((uint64_t)PAGE_SIZE << buf_order); ++i)
        buf_va[i] = 0;

    if (vq_init(&d->q, d->io_base, buf_va, buf_phys) != 0) {
        return -1;
    }

    /* DRIVER_OK */
    outb_(d->io_base + VIRTIO_PCI_STATUS,
          VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
    VIRTIO_MB();

    /* 填 block_device */
    struct block_device *bd = &d->bdev;
    const char *nm = "vda";
    for (int i = 0; i < 16 && nm[i]; ++i) bd->name[i] = nm[i];
    bd->name[3] = '\0';
    bd->sector_size    = BLOCK_SECTOR_SIZE;
    bd->total_sectors  = d->capacity_sectors;
    bd->start_lba      = 0;
    bd->read           = vblk_read_sectors;
    bd->write          = vblk_write_sectors;
    bd->driver_data    = d;

    if (block_register_device(bd) != 0) {
        serial_printf("[VIRTIO-BLK] register failed\n");
        return -1;
    }

    d->ready = 1;
    serial_printf("[VIRTIO-BLK] ready: %s sectors=%llu\n",
                  bd->name, (unsigned long long)bd->total_sectors);
    return 0;
}

int virtio_blk_ready(void) { return g_vblk.ready; }

struct block_device *virtio_blk_device(void)
{
    return g_vblk.ready ? &g_vblk.bdev : 0;
}
/*===OmniBridgeOs/kernel/arch/x64/block/virtio_blk.c 结束===*/