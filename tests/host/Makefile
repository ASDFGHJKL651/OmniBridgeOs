#OmniBridge/tests/host/Makefile
CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 \
           -I ../../kernel/arch/x64 \
           -I ../../kernel/arch/x64/net \
           -I ../../kernel/arch/x64/block

TESTS := test_serial test_pmm test_kmalloc test_vmm test_slab test_obfs_logic

.PHONY: all clean run

all: $(TESTS)

test_serial: test_serial.c ../../kernel/arch/x64/kprintf.c
	$(CC) $(CFLAGS) $^ -o $@

# 链接真实 printk.c + pmm.c；由 stubs.c 提供 serial_* 桩。
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

# VMM 纯逻辑测试：只验证宏与索引计算，不链接内核代码。
test_vmm: test_vmm.c
	$(CC) $(CFLAGS) $^ -o $@

# SLAB 布局数学的纯逻辑测试。
test_slab: test_slab.c
	$(CC) $(CFLAGS) $^ -o $@

# ★ 第 18B 步：OBFS 布局/日志校验和/位图 纯逻辑测试
test_obfs_logic: test_obfs_logic.c
	$(CC) $(CFLAGS) $^ -o $@

run: all
	@for t in $(TESTS); do echo "== $$t =="; ./$$t || exit 1; done

clean:
	rm -f $(TESTS) *.o