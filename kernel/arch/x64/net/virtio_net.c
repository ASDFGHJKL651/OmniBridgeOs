/*===OmniBridgeOs/kernel/arch/x64/net/virtio_net.c===*/
/*
 * VirtIO 网卡驱动（第 18A 步）。
 *
 * 修复历史：
 *   v1-v4：vring 布局、描述符回收、RX notify、截断队列大小。
 *   v5：MAC 用 I/O 端口读；屏障；越界消费；notify；netbuf 泄漏。
 *   v6：__atomic_* 访问 DMA 共享字段（后被 v7 弃用）。
 *   v7：volatile + lfence/sfence/mfence，避免 libatomic 依赖。
 *   v8：协商 VIRTIO_NET_F_MAC。
 *   v9：全链路诊断打印（MAC/PFN/Features 回读）。
 *   v10（本轮，最终修复）：
 *     ★ 补齐 virtio_net_hdr 前缀（VIRTIO 1.0 §5.1.6）。
 *
 *       根因：QEMU 的 virtio-net 在 virtio_net_flush_tx() 中会**无条件
 *       丢弃** TX 缓冲区的前 sizeof(struct virtio_net_hdr) = 10 字节
 *       （因为未协商 MRG_RXBUF）。我们直接把以太网帧放在偏移 0，于是
 *       SLIRP 看到的"以太网帧"从原 src MAC 的第 3 字节开始，目的 MAC
 *       完全是垃圾，SLIRP 静默丢弃所有 DHCP DISCOVER。
 *       同理，RX 侧设备写回的数据也是 [hdr(10) | packet]，驱动必须
 *       跳过前 10 字节。
 *
 *       症状完全符合日志：
 *         - txq.used->idx 正常推进（设备正常消费了描述符）
 *         - rxq.used->idx 恒为 0（后端从未产生入站流量）
 *         - MAC / PFN / features 全部正确
 *
 * vring 物理布局（规范要求，num 为设备报告的队列大小）：
 *   vring_pa + 0                     desc 表（16 * num 字节）
 *   vring_pa + 16*num                avail ring（4 + 2*num 字节）
 *   vring_pa + align4k(...)          used ring（4 + 8*num 字节，4K 对齐）
 *
 * 数据缓冲区布局（order=4，64KB）：
 *   [0K  .. 32K)  RX 数据缓冲：16 × 2KB
 *   [32K .. 64K)  TX 数据缓冲：16 × 2KB
 *
 *   ★ v10：每个 2KB 缓冲区的实际布局为：
 *       [ virtio_net_hdr (10) | 以太网帧 (<= 2038) ]
 *     前 10 字节 TX 时清零，RX 时由设备填充并被驱动跳过。
 */
#include "virtio_net.h"
#include "pci.h"
#include "ethernet.h"
#include "pmm.h"
#include "vmm.h"
#include "kmalloc.h"
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

/* ---------- virtio-net 特性位 ---------- */
#define VIRTIO_NET_F_MAC        5    /* bit 5 */
#define VIRTIO_NET_F_MAC_BIT    (1u << VIRTIO_NET_F_MAC)

/* ---------- ★ v10：virtio_net_hdr ---------- */
/*
 * 未协商 VIRTIO_NET_F_MRG_RXBUF 时，TX/RX 缓冲区都必须以
 * sizeof(struct virtio_net_hdr) = 10 字节的头部开始。
 * 我们未协商任何 TSO / CSUM 特性，因此该头部全部清零即可。
 * （保留结构体定义供将来扩展 TSO/校验和卸载使用。）
 */
#define VIRTIO_NET_HDR_LEN 10
struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed));

/* ---------- vring 描述符标志 ---------- */
#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

/* 驱动实际使用的描述符数量（受缓冲区数量限制） */
#define VQ_SIZE     16
/* vring 结构体数组的编译期最大容量（QEMU legacy virtio-net 为 256） */
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
    int      is_rx;
    int      ready;
};

struct vdev {
    uint16_t io_base;
    uint8_t  mac[6];
    uint8_t  _pad[2];
    struct vq rxq;
    struct vq txq;
    struct netif nif;
    int      ready;
};

static struct vdev g_vdev;
static spinlock_t  g_lock = SPINLOCK_INIT;

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

/* ---------- 屏障宏 ---------- */
#define VIRTIO_RMB()  __asm__ __volatile__("lfence" ::: "memory")
#define VIRTIO_WMB()  __asm__ __volatile__("sfence" ::: "memory")
#define VIRTIO_MB()   __asm__ __volatile__("mfence" ::: "memory")

/* ---------- DMA 共享字段访问 ---------- */
static inline uint16_t used_idx_load(const struct vq *q)
{ return *(volatile const uint16_t *)(const void *)&q->used->idx; }

static inline uint32_t used_ring_id_load(const struct vq *q, uint16_t i)
{ return *(volatile const uint32_t *)(const void *)&q->used->ring[i].id; }

static inline uint32_t used_ring_len_load(const struct vq *q, uint16_t i)
{ return *(volatile const uint32_t *)(const void *)&q->used->ring[i].len; }

static inline uint16_t avail_idx_load(const struct vq *q)
{ return *(volatile const uint16_t *)(const void *)&q->avail->idx; }

static inline void avail_idx_store(struct vq *q, uint16_t v)
{ *(volatile uint16_t *)(void *)&q->avail->idx = v; }

static inline void avail_ring_store(struct vq *q, uint16_t i, uint16_t v)
{ *(volatile uint16_t *)(void *)&q->avail->ring[i] = v; }

/* ============================================================
 * 队列初始化
 * ============================================================ */
static void vq_init(struct vq *q, uint16_t io_base, uint16_t idx,
                    uint8_t *buf_mem, uint64_t buf_phys, int is_rx)
{
    outw_(io_base + VIRTIO_PCI_QUEUE_SEL, idx);
    uint16_t full_num = inw_(io_base + VIRTIO_PCI_QUEUE_NUM);
    if (full_num == 0) {
        serial_printf("[VIRTIO-NET] vq%u: queue_num=0\n", (unsigned)idx);
        q->ready = 0;
        return;
    }
    if (full_num > VQ_MAX_NUM) full_num = VQ_MAX_NUM;

    uint16_t use_num = (full_num < VQ_SIZE) ? full_num : VQ_SIZE;

    q->num        = full_num;
    q->use_num    = use_num;
    q->last_used  = 0;
    q->free_head  = 0;
    q->is_rx      = is_rx;
    q->ready      = 0;

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
    if (order > 8) {
        serial_printf("[VIRTIO-NET] vq%u: vring too large (%u bytes)\n",
                      (unsigned)idx, (unsigned)total);
        return;
    }

    struct page *pg = pmm_alloc_pages(order);
    if (!pg) {
        serial_printf("[VIRTIO-NET] vq%u: pmm_alloc_pages(order=%d) failed\n",
                      (unsigned)idx, order);
        return;
    }
    uint64_t pa = page_to_phys(pg);
    uint8_t *va = (uint8_t *)(DIRECTMAP_BASE + pa);
    uint64_t alloc_bytes = (uint64_t)PAGE_SIZE << order;
    for (uint64_t i = 0; i < alloc_bytes; ++i) va[i] = 0;

    q->vring_pa = pa;
    q->desc     = (struct vring_desc  *)(va + desc_off);
    q->avail    = (struct vring_avail *)(va + avail_off);
    q->used     = (struct vring_used  *)(va + used_off);

    q->bufs      = buf_mem;
    q->phys_bufs = buf_phys;

    /* 每个描述符对应一个完整的 NETBUF_DATA_SIZE 缓冲区。
     * RX：缓冲区容量 = NETBUF_DATA_SIZE，包含 hdr(10) + 数据。
     * TX：缓冲区容量 = NETBUF_DATA_SIZE，包含 hdr(10) + 数据。 */
    for (uint16_t i = 0; i < use_num; ++i) {
        q->desc[i].addr  = buf_phys + (uint64_t)i * NETBUF_DATA_SIZE;
        q->desc[i].len   = NETBUF_DATA_SIZE;
        q->desc[i].flags = is_rx ? VRING_DESC_F_WRITE : 0;
        q->desc[i].next  = (uint16_t)((i + 1) % use_num);
    }
    if (use_num > 0) q->desc[use_num - 1].next = 0;

    q->avail->flags = 0;

    if (is_rx) {
        for (uint16_t i = 0; i < use_num; ++i)
            avail_ring_store(q, i, i);
        VIRTIO_WMB();
        avail_idx_store(q, use_num);
        q->num_free   = 0;
    } else {
        avail_idx_store(q, 0);
        q->num_free   = use_num;
    }

    uint32_t pfn = (uint32_t)(pa >> 12);
    outl_(io_base + VIRTIO_PCI_QUEUE_PFN, pfn);
    VIRTIO_MB();
    uint32_t pfn_rb = inl_(io_base + VIRTIO_PCI_QUEUE_PFN);
    if (pfn_rb != pfn) {
        serial_printf("[VIRTIO-NET] WARN vq%u: PFN readback mismatch "
                      "(wrote=0x%x read=0x%x)\n",
                      (unsigned)idx, (unsigned)pfn, (unsigned)pfn_rb);
    } else {
        serial_printf("[VIRTIO-NET] vq%u: PFN=0x%x accepted\n",
                      (unsigned)idx, (unsigned)pfn);
    }

    serial_printf("[VIRTIO-NET] vq%u: dev_num=%u use=%u vring=0x%llx "
                  "desc=+%u avail=+%u used=+%u total=%u order=%d "
                  "hdr_len=%d\n",
                  (unsigned)idx,
                  (unsigned)full_num,
                  (unsigned)use_num,
                  (unsigned long long)pa,
                  (unsigned)desc_off,
                  (unsigned)avail_off,
                  (unsigned)used_off,
                  (unsigned)total,
                  order,
                  VIRTIO_NET_HDR_LEN);

    q->ready = 1;
}

static void vdev_set_status(struct vdev *d, uint8_t s)
{
    outb_(d->io_base + VIRTIO_PCI_STATUS, s);
    VIRTIO_MB();
}

static uint8_t vdev_get_status(struct vdev *d)
{
    return inb_(d->io_base + VIRTIO_PCI_STATUS);
}

/* ============================================================
 * 特性协商
 * ============================================================ */
static int vdev_negotiate(struct vdev *d)
{
    vdev_set_status(d, 0);
    vdev_set_status(d, VIRTIO_STATUS_ACK);
    vdev_set_status(d, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    uint32_t host_feat  = inl_(d->io_base + VIRTIO_PCI_HOST_FEATURES);
    uint32_t guest_feat = 0;

    /* ★ 只协商 MAC：不协商 MRG_RXBUF（保持 hdr_len = 10），
     * 不协商任何 TSO / CSUM 特性（避免 virtio_net_hdr 里的
     * flags/gso_type/csum_* 字段需要填值）。 */
    if (host_feat & VIRTIO_NET_F_MAC_BIT) {
        guest_feat |= VIRTIO_NET_F_MAC_BIT;
    }

    serial_printf("[VIRTIO-NET] features: host=0x%08x guest=0x%08x "
                  "(MAC=%s, hdr_len=%d)\n",
                  (unsigned)host_feat,
                  (unsigned)guest_feat,
                  (guest_feat & VIRTIO_NET_F_MAC_BIT) ? "negotiated"
                                                      : "NOT available",
                  VIRTIO_NET_HDR_LEN);

    outl_(d->io_base + VIRTIO_PCI_GUEST_FEATURES, guest_feat);
    VIRTIO_MB();

    uint32_t guest_rb = inl_(d->io_base + VIRTIO_PCI_GUEST_FEATURES);
    if (guest_rb != guest_feat) {
        serial_printf("[VIRTIO-NET] WARN: guest features readback mismatch "
                      "(wrote=0x%08x read=0x%08x)\n",
                      (unsigned)guest_feat, (unsigned)guest_rb);
    }

    uint8_t st = vdev_get_status(d);
    if (st & VIRTIO_STATUS_FAILED) {
        serial_printf("[VIRTIO-NET] ERROR: device status FAILED (0x%x)\n",
                      (unsigned)st);
        return -1;
    }
    return 0;
}

static void vdev_read_mac(struct vdev *d)
{
    for (int i = 0; i < 6; ++i) {
        d->mac[i] = inb_(d->io_base + VIRTIO_PCI_CONFIG + i);
    }

    serial_printf("[VIRTIO-NET] MAC from config: "
                  "%02x:%02x:%02x:%02x:%02x:%02x\n",
                  d->mac[0], d->mac[1], d->mac[2],
                  d->mac[3], d->mac[4], d->mac[5]);

    int all_zero = (d->mac[0] == 0 && d->mac[1] == 0 && d->mac[2] == 0 &&
                    d->mac[3] == 0 && d->mac[4] == 0 && d->mac[5] == 0);
    if (all_zero) {
        serial_printf("[VIRTIO-NET] WARN: MAC is all-zero, "
                      "using fallback 52:54:00:12:34:56\n");
        d->mac[0] = 0x52; d->mac[1] = 0x54;
        d->mac[2] = 0x00; d->mac[3] = 0x12;
        d->mac[4] = 0x34; d->mac[5] = 0x56;
    }
}

/* ============================================================
 * 诊断打印
 * ============================================================ */
static void vq_dump(const char *tag, const struct vq *q)
{
    uint16_t used_idx = used_idx_load(q);
    serial_printf("[VIRTIO-NET-DBG] %s: num=%u use=%u free_head=%u "
                  "num_free=%u last_used=%u used->idx=%u\n",
                  tag,
                  (unsigned)q->num,
                  (unsigned)q->use_num,
                  (unsigned)q->free_head,
                  (unsigned)q->num_free,
                  (unsigned)q->last_used,
                  (unsigned)used_idx);
}

void virtio_net_debug_dump(struct netif *nif)
{
    if (!nif) return;
    struct vdev *d = (struct vdev *)nif->driver;
    if (!d || !d->ready) return;
    uint64_t fl;
    spin_lock_irqsave(&g_lock, &fl);
    vq_dump("rxq", &d->rxq);
    vq_dump("txq", &d->txq);
    spin_unlock_irqrestore(&g_lock, fl);
}

/* ============================================================
 * ★ v10：TX 路径 —— 前置 virtio_net_hdr
 * ============================================================ */
static int virtio_xmit(struct netif *nif, struct netbuf *nb)
{
    struct vdev *d = (struct vdev *)nif->driver;
    if (!d || !d->ready) { netbuf_free(nb); return -1; }

    uint64_t fl;
    spin_lock_irqsave(&g_lock, &fl);
    struct vq *q = &d->txq;
    if (!q->ready) {
        spin_unlock_irqrestore(&g_lock, fl);
        netbuf_free(nb);
        return -1;
    }

    /* ---- 回收设备已处理的 TX 描述符 ---- */
    for (;;) {
        uint16_t used_idx = used_idx_load(q);
        if (q->last_used == used_idx) break;

        uint16_t uidx = (uint16_t)(q->last_used % q->num);
        uint32_t did  = used_ring_id_load(q, uidx);
        q->last_used++;

        if (did >= q->use_num) continue;

        q->desc[did].next = q->free_head;
        q->free_head = (uint16_t)did;
        q->num_free++;
    }

    if (q->num_free < 1) {
        spin_unlock_irqrestore(&g_lock, fl);
        netbuf_free(nb);
        return -1;
    }

    uint16_t idx = q->free_head;
    q->free_head = q->desc[idx].next;
    q->desc[idx].next = 0;
    q->num_free--;

    /* ---- ★ v10：填充 [virtio_net_hdr(10) | ethernet frame] ---- */
    uint8_t *buf = q->bufs + (uint64_t)idx * NETBUF_DATA_SIZE;
    uint32_t max_data = NETBUF_DATA_SIZE - VIRTIO_NET_HDR_LEN;
    uint32_t copy = nb->len;
    if (copy > max_data) copy = max_data;

    /* 前 10 字节清零作为 virtio_net_hdr（未协商任何 TSO/CSUM 特性） */
    for (int i = 0; i < VIRTIO_NET_HDR_LEN; ++i) buf[i] = 0;
    /* 以太网帧紧随其后 */
    for (uint32_t i = 0; i < copy; ++i)
        buf[VIRTIO_NET_HDR_LEN + i] = nb->data[i];

    q->desc[idx].len   = VIRTIO_NET_HDR_LEN + copy;
    q->desc[idx].flags = 0;

    /* ---- 提交到 avail ring：desc → ring → idx → notify ---- */
    VIRTIO_WMB();
    avail_ring_store(q, (uint16_t)(avail_idx_load(q) % q->num), idx);
    VIRTIO_WMB();
    avail_idx_store(q, (uint16_t)(avail_idx_load(q) + 1));

    VIRTIO_MB();
    outw_(d->io_base + VIRTIO_PCI_QUEUE_NOTIFY, 1);

    spin_unlock_irqrestore(&g_lock, fl);
    netbuf_free(nb);
    return 0;
}

/* ============================================================
 * ★ v10：RX 轮询 —— 跳过 virtio_net_hdr
 * ============================================================ */
void virtio_net_poll(struct netif *nif)
{
    if (!nif) return;
    struct vdev *d = (struct vdev *)nif->driver;
    if (!d || !d->ready) return;

    uint64_t fl;
    spin_lock_irqsave(&g_lock, &fl);
    struct vq *q = &d->rxq;
    if (!q->ready) {
        spin_unlock_irqrestore(&g_lock, fl);
        return;
    }

    int appended = 0;

    for (;;) {
        uint16_t used_idx = used_idx_load(q);
        if (q->last_used == used_idx) break;

        uint16_t uidx    = (uint16_t)(q->last_used % q->num);
        uint32_t desc_id = used_ring_id_load(q, uidx);
        uint32_t dlen    = used_ring_len_load(q, uidx);
        q->last_used++;

        if (desc_id >= q->use_num) continue;

        uint8_t *buf = q->bufs + (uint64_t)desc_id * NETBUF_DATA_SIZE;

        /* 无条件把描述符挂回 avail ring */
        q->desc[desc_id].flags = VRING_DESC_F_WRITE;
        q->desc[desc_id].len   = NETBUF_DATA_SIZE;
        q->desc[desc_id].next  = 0;
        VIRTIO_WMB();
        avail_ring_store(q, (uint16_t)(avail_idx_load(q) % q->num),
                         (uint16_t)desc_id);
        VIRTIO_WMB();
        avail_idx_store(q, (uint16_t)(avail_idx_load(q) + 1));
        appended = 1;

        /* ★ v10：dlen 包含 virtio_net_hdr(10)，必须跳过 */
        if (dlen > VIRTIO_NET_HDR_LEN && dlen <= NETBUF_DATA_SIZE) {
            uint32_t pktlen = dlen - VIRTIO_NET_HDR_LEN;
            struct netbuf *nb = netbuf_alloc(nif->ns, pktlen);
            if (nb) {
                const uint8_t *src = buf + VIRTIO_NET_HDR_LEN;
                for (uint32_t i = 0; i < pktlen; ++i)
                    nb->data[i] = src[i];
                nb->len = pktlen;

                spin_unlock_irqrestore(&g_lock, fl);
                ethernet_rx(nif, nb);
                spin_lock_irqsave(&g_lock, &fl);
            }
        } else if (dlen == VIRTIO_NET_HDR_LEN) {
            /* 只有 hdr，无数据，丢弃 */
        }
    }

    if (appended) {
        VIRTIO_MB();
        outw_(d->io_base + VIRTIO_PCI_QUEUE_NOTIFY, 0);
    }

    spin_unlock_irqrestore(&g_lock, fl);
}

/* ============================================================
 * 别名接口
 * ============================================================ */
int virtio_net_send(struct netif *nif, struct netbuf *nb)
{
    return virtio_xmit(nif, nb);
}

void virtio_net_recv(struct netif *nif)
{
    virtio_net_poll(nif);
}

/* ============================================================
 * 初始化
 * ============================================================ */
int virtio_net_init(struct netns *ns)
{
    struct pci_device dev;
    if (pci_find_device(PCI_VENDOR_VIRTIO, 0xFFFF, &dev) != 0) {
        serial_printf("[VIRTIO-NET] no VirtIO device found\n");
        return -1;
    }
    if (dev.device != PCI_DEV_VIRTIO_NET_LEGACY &&
        dev.device != PCI_DEV_VIRTIO_NET_MODERN) {
        serial_printf("[VIRTIO-NET] found VirtIO dev=0x%04x (not net)\n",
                      (unsigned)dev.device);
        return -1;
    }

    serial_printf("[PCI] virtio-net found %04x:%04x bus=%u slot=%u func=%u\n",
                  (unsigned)dev.vendor, (unsigned)dev.device,
                  (unsigned)dev.bus, (unsigned)dev.slot, (unsigned)dev.func);

    uint64_t base = 0, size = 0;
    uint8_t is_io = 0;
    if (pci_read_bar(&dev, 0, &base, &size, &is_io) != 0 || !is_io) {
        serial_printf("[VIRTIO-NET] BAR0 not I/O\n");
        return -1;
    }

    pci_enable_bus_master(&dev);

    struct vdev *d = &g_vdev;
    uint8_t *p = (uint8_t *)d;
    for (unsigned i = 0; i < sizeof(*d); ++i) p[i] = 0;
    d->io_base = (uint16_t)base;
    d->ready   = 0;
    spin_lock_init(&g_lock);

    serial_printf("[VIRTIO-NET] io_base=0x%x size=0x%llx\n",
                  (unsigned)d->io_base, (unsigned long long)size);

    int dma_order = 4;
    struct page *pg = pmm_alloc_pages(dma_order);
    if (!pg) {
        serial_printf("[VIRTIO-NET] DMA alloc failed\n");
        return -1;
    }
    uint64_t buf_phys = page_to_phys(pg);
    uint8_t *buf_va = (uint8_t *)(DIRECTMAP_BASE + buf_phys);
    for (uint64_t i = 0; i < (PAGE_SIZE << dma_order); ++i) buf_va[i] = 0;

    if (vdev_negotiate(d) != 0) {
        serial_printf("[VIRTIO-NET] negotiate failed\n");
        return -1;
    }

    uint8_t *rxbufs = buf_va;
    uint8_t *txbufs = buf_va + 16 * NETBUF_DATA_SIZE;
    vq_init(&d->rxq, d->io_base, 0, rxbufs, buf_phys, 1);
    vq_init(&d->txq, d->io_base, 1, txbufs,
            buf_phys + 16 * NETBUF_DATA_SIZE, 0);

    if (!d->rxq.ready || !d->txq.ready) {
        serial_printf("[VIRTIO-NET] queue init failed (rx=%d tx=%d)\n",
                      d->rxq.ready, d->txq.ready);
        return -1;
    }

    vdev_read_mac(d);

    vdev_set_status(d, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                        VIRTIO_STATUS_DRIVER_OK);

    /* DRIVER_OK 之后重新 select RX queue 并重新写 PFN */
    outw_(d->io_base + VIRTIO_PCI_QUEUE_SEL, 0);
    outl_(d->io_base + VIRTIO_PCI_QUEUE_PFN,
          (uint32_t)(d->rxq.vring_pa >> 12));
    VIRTIO_MB();

    for (int i = 0; i < 3; ++i) {
        outw_(d->io_base + VIRTIO_PCI_QUEUE_NOTIFY, 0);
        VIRTIO_MB();
    }

    struct netif *nif = &d->nif;
    p = (uint8_t *)nif;
    for (unsigned i = 0; i < sizeof(*nif); ++i) p[i] = 0;
    const char *nm = "eth0";
    int i = 0; while (nm[i]) { nif->name[i] = nm[i]; ++i; } nif->name[i] = '\0';
    for (int k = 0; k < 6; ++k) nif->mac[k] = d->mac[k];
    nif->up     = 1;
    nif->ns     = ns;
    nif->driver = d;
    nif->xmit   = virtio_xmit;
    nif->poll   = virtio_net_poll;

    if (ns) netns_add_iface(ns, nif);

    d->ready = 1;
    serial_printf("[VIRTIO-NET] queues ready MAC=%02x:%02x:%02x:%02x:%02x:%02x "
                  "rxq.num=%u rxq.use=%u txq.num=%u txq.use=%u\n",
                  d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5],
                  (unsigned)d->rxq.num, (unsigned)d->rxq.use_num,
                  (unsigned)d->txq.num, (unsigned)d->txq.use_num);

    vq_dump("rxq init", &d->rxq);
    vq_dump("txq init", &d->txq);

    uint8_t isr = inb_(d->io_base + VIRTIO_PCI_ISR);
    serial_printf("[VIRTIO-NET] ISR after init = 0x%x\n", (unsigned)isr);

    return 0;
}
/*===OmniBridgeOs/kernel/arch/x64/net/virtio_net.c 结束===*/