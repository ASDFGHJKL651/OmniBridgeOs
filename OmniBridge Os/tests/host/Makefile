#OmniBridge/tests/host/Makefile
CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 \
           -I ../../kernel/arch/x64

TESTS := test_serial test_pmm test_kmalloc test_vmm test_slab

.PHONY: all clean run

all: $(TESTS)

test_serial: test_serial.c ../../kernel/arch/x64/kprintf.c
	$(CC) $(CFLAGS) $^ -o $@

# 链接真实 printk.c + pmm.c；由 stubs.c 提供 serial_* 桩。
# 注意：pmm.c 现在引用 spinlock.h（纯内联），无需额外目标文件；
# 但仍需要 printk.c / kprintf.c 以解析 printk 与 serial_printf 符号。
test_pmm: test_pmm.c ../../kernel/arch/x64/pmm.c \
          ../../kernel/arch/x64/printk.c \
          ../../kernel/arch/x64/kprintf.c \
          stubs.c
	$(CC) $(CFLAGS) $^ -o $@

# 链接真实 kmalloc.c；kmalloc.c 现在依赖 slab.c 提供的
# kmem_cache_* 与 slab_init，因此必须一并链接 slab.c。
test_kmalloc: test_kmalloc.c ../../kernel/arch/x64/kmalloc.c \
              ../../kernel/arch/x64/slab.c \
              ../../kernel/arch/x64/pmm.c \
              ../../kernel/arch/x64/printk.c \
              ../../kernel/arch/x64/kprintf.c \
              stubs.c
	$(CC) $(CFLAGS) $^ -o $@

test_vmm: test_vmm.c
	$(CC) $(CFLAGS) $^ -o $@

# 新增：SLAB 布局数学的纯逻辑测试（不链接内核代码）
test_slab: test_slab.c
	$(CC) $(CFLAGS) $^ -o $@

run: all
	@for t in $(TESTS); do echo "== $$t =="; ./$$t || exit 1; done

clean:
	rm -f $(TESTS) *.o