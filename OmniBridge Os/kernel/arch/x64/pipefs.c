/*===OmniBridgeOs/kernel/arch/x64/pipefs.c===*/
#include "pipefs.h"
#include "kmalloc.h"
#include "serial.h"
#include "sched.h"
#include "task.h"
#include "vmm.h"
#include "user/user.h"
#include "select.h"

static int64_t pipe_read_op(struct vfs_file *f, void *buf, uint64_t n)
{ return pipefs_read(f, buf, n); }
static int64_t pipe_write_op(struct vfs_file *f, const void *buf, uint64_t n)
{ return pipefs_write(f, buf, n); }
static int pipe_close_op(struct vfs_file *f)
{ return pipefs_close(f); }
static int pipe_poll_op(struct vfs_file *f, short events)
{ return pipefs_poll(f, events); }

static const struct vfs_operations g_pipe_ops = {
    .read  = pipe_read_op,
    .write = pipe_write_op,
    .close = pipe_close_op,
    .poll  = pipe_poll_op,
};

int pipefs_init(void)
{
    serial_printf("[PIPEFS] init: buf=%u\n", (unsigned)PIPE_BUF_SIZE);
    return 0;
}

static void wait_push(struct pipe_wait_node **head, uint32_t *cnt,
                      struct thread *th)
{
    struct pipe_wait_node *n =
        (struct pipe_wait_node *)kzalloc(sizeof(*n));
    if (!n) return;
    n->th   = th;
    n->next = *head;
    *head   = n;
    (*cnt)++;
}

static void wait_wake_all(struct pipe_wait_node **head, uint32_t *cnt)
{
    extern void sched_enqueue_thread(struct thread *th);
    struct pipe_wait_node *n;
    while ((n = *head) != 0) {
        *head = n->next;
        if (*cnt > 0) (*cnt)--;
        if (n->th) sched_enqueue_thread(n->th);
        kfree(n);
    }
}

static void pipe_free(struct pipe_inode *p)
{
    if (!p) return;
    if (p->data) { kfree(p->data); p->data = 0; }
    kfree(p);
}

int64_t pipefs_read(struct vfs_file *f, void *buf, uint64_t count)
{
    if (!f || !buf) return -22;
    struct pipe_inode *p = (struct pipe_inode *)f->private_data;
    if (!p) return -5;

    for (;;) {
        uint64_t fl;
        spin_lock_irqsave(&p->lock, &fl);

        if (p->len > 0) {
            uint32_t take = (uint32_t)((count < p->len) ? count : p->len);
            uint8_t *d = (uint8_t *)buf;
            for (uint32_t i = 0; i < take; ++i) {
                d[i] = p->data[p->head];
                p->head = (p->head + 1) % PIPE_BUF_SIZE;
            }
            p->len -= take;

            int need_wake_writers = (p->write_wait_count > 0);
            spin_unlock_irqrestore(&p->lock, fl);

            if (need_wake_writers)
                wait_wake_all(&p->write_waiters, &p->write_wait_count);

            /* ★ 修复 3：通知 poll 等待者 */
            select_notify_wakeup();

            return (int64_t)take;
        }

        if (p->writers == 0) {
            spin_unlock_irqrestore(&p->lock, fl);
            return 0;
        }

        struct thread *self = sched_current();
        if (!self) { spin_unlock_irqrestore(&p->lock, fl); return -1; }
        wait_push(&p->read_waiters, &p->read_wait_count, self);
        self->state = THREAD_STATE_BLOCKED;
        spin_unlock_irqrestore(&p->lock, fl);

        schedule();
    }
}

int64_t pipefs_write(struct vfs_file *f, const void *buf, uint64_t count)
{
    if (!f || !buf) return -22;
    struct pipe_inode *p = (struct pipe_inode *)f->private_data;
    if (!p) return -5;

    uint64_t written = 0;
    const uint8_t *s = (const uint8_t *)buf;

    while (written < count) {
        uint64_t fl;
        spin_lock_irqsave(&p->lock, &fl);

        if (p->readers == 0) {
            spin_unlock_irqrestore(&p->lock, fl);
            return (written > 0) ? (int64_t)written : -32;
        }

        if (p->len < PIPE_BUF_SIZE) {
            uint32_t space = PIPE_BUF_SIZE - p->len;
            uint32_t take  = (uint32_t)((count - written) < space ?
                                         (count - written) : space);
            for (uint32_t i = 0; i < take; ++i) {
                p->data[p->tail] = s[written + i];
                p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
            }
            p->len += take;
            written += take;

            int need_wake_readers = (p->read_wait_count > 0);
            spin_unlock_irqrestore(&p->lock, fl);

            if (need_wake_readers)
                wait_wake_all(&p->read_waiters, &p->read_wait_count);

            /* ★ 修复 3：通知 poll 等待者 */
            select_notify_wakeup();

            continue;
        }

        struct thread *self = sched_current();
        if (!self) { spin_unlock_irqrestore(&p->lock, fl); return -1; }
        wait_push(&p->write_waiters, &p->write_wait_count, self);
        self->state = THREAD_STATE_BLOCKED;
        spin_unlock_irqrestore(&p->lock, fl);

        schedule();
    }

    return (int64_t)written;
}

int pipefs_close(struct vfs_file *f)
{
    if (!f) return -22;
    struct pipe_inode *p = (struct pipe_inode *)f->private_data;
    if (!p) return 0;

    uint64_t fl;
    spin_lock_irqsave(&p->lock, &fl);

    int is_reader = (f->f_flags & VFS_O_WRONLY) == 0;
    if (is_reader) {
        if (p->readers > 0) p->readers--;
    } else {
        if (p->writers > 0) p->writers--;
    }

    uint32_t r = p->readers;
    uint32_t w = p->writers;
    spin_unlock_irqrestore(&p->lock, fl);

    if (r == 0)
        wait_wake_all(&p->write_waiters, &p->write_wait_count);
    if (w == 0)
        wait_wake_all(&p->read_waiters, &p->read_wait_count);

    /* ★ 修复 3：通知 poll 等待者 */
    select_notify_wakeup();

    if (r == 0 && w == 0) {
        pipe_free(p);
    }

    if (f->private_data) f->private_data = 0;
    return 0;
}

int pipefs_poll(struct vfs_file *f, short events)
{
    if (!f) return 0;
    struct pipe_inode *p = (struct pipe_inode *)f->private_data;
    if (!p) return 0;

    short rev = 0;
    uint64_t fl;
    spin_lock_irqsave(&p->lock, &fl);

    int is_reader = (f->f_flags & VFS_O_WRONLY) == 0;
    if (is_reader) {
        if (p->len > 0) rev |= POLLIN;
        if (p->writers == 0) rev |= POLLHUP;
    } else {
        if (p->len < PIPE_BUF_SIZE) rev |= POLLOUT;
        if (p->readers == 0) rev |= POLLERR;
    }
    spin_unlock_irqrestore(&p->lock, fl);
    return (int)rev;
}

int pipefs_create(struct task_t *cur, uint64_t uaddr)
{
    if (!cur) return -1;

    struct pipe_inode *p = (struct pipe_inode *)kzalloc(sizeof(*p));
    if (!p) return -12;
    p->data = (uint8_t *)kmalloc(PIPE_BUF_SIZE);
    if (!p->data) { kfree(p); return -12; }
    spin_lock_init(&p->lock);
    p->readers = 1;
    p->writers = 1;

    struct vfs_file *rf = (struct vfs_file *)kzalloc(sizeof(*rf));
    struct vfs_file *wf = (struct vfs_file *)kzalloc(sizeof(*wf));
    if (!rf || !wf) {
        if (rf) kfree(rf);
        if (wf) kfree(wf);
        pipe_free(p);
        return -12;
    }

    struct vfs_inode *ino = (struct vfs_inode *)kzalloc(sizeof(*ino));
    if (!ino) {
        kfree(rf); kfree(wf);
        pipe_free(p);
        return -12;
    }
    ino->mode = VFS_S_IFIFO | 0600;
    ino->ops  = &g_pipe_ops;

    rf->f_inode = ino;
    rf->f_flags = VFS_O_RDONLY;
    rf->private_data = p;

    wf->f_inode = ino;
    wf->f_flags = VFS_O_WRONLY;
    wf->private_data = p;

    int fd_r = -1, fd_w = -1;
    for (int i = 3; i < 64; ++i) {
        if (!cur->fd_table[i]) {
            if (fd_r < 0) { fd_r = i; }
            else if (fd_w < 0) { fd_w = i; break; }
        }
    }
    if (fd_r < 0 || fd_w < 0) {
        kfree(ino); kfree(rf); kfree(wf);
        pipe_free(p);
        return -11;
    }

    cur->fd_table[fd_r] = rf;
    cur->fd_table[fd_w] = wf;

    int fds[2] = { fd_r, fd_w };
    uint64_t pa64 = 0;
    {
        struct user_ctx *uc = user_get_ctx(cur);
        uint64_t *pml4 = 0;
        if (cur->priv_iso_ready && cur->mem_domain.pml4_self_ptr) {
            pml4 = cur->mem_domain.pml4_self_ptr;
        } else if (uc && uc->pml4) {
            pml4 = uc->pml4;
        }
        if (!pml4) return -1;
        uint64_t *pte = vmm_get_pte(pml4, uaddr);
        if (!pte || !(*pte & PTE_PRESENT) || !(*pte & PTE_USER)) return -14;
        pa64 = (*pte & PTE_ADDR_MASK) + (uaddr & 0xFFF);
    }
    int *dst = (int *)(uintptr_t)(DIRECTMAP_BASE + pa64);
    dst[0] = fds[0];
    dst[1] = fds[1];

    serial_printf("[PIPEFS] created pipe rd=%d wr=%d\n", fd_r, fd_w);
    return 0;
}
/*===OmniBridgeOs/kernel/arch/x64/pipefs.c 结束===*/