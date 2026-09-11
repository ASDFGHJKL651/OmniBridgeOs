CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 \
           -I ../../kernel/arch/x64

TESTS := test_serial test_pmm test_kmalloc test_vmm

.PHONY: all clean run

all: $(TESTS)

test_serial: test_serial.c ../../kernel/arch/x64/kprintf.c
	$(CC) $(CFLAGS) $^ -o $@

# 链接真实 printk.c，由 stubs.c 提供 serial_* 桩
test_pmm: test_pmm.c ../../kernel/arch/x64/pmm.c \
          ../../kernel/arch/x64/printk.c \
          ../../kernel/arch/x64/kprintf.c \
          stubs.c
	$(CC) $(CFLAGS) $^ -o $@

test_kmalloc: test_kmalloc.c ../../kernel/arch/x64/kmalloc.c \
              ../../kernel/arch/x64/pmm.c \
              ../../kernel/arch/x64/printk.c \
              ../../kernel/arch/x64/kprintf.c \
              stubs.c
	$(CC) $(CFLAGS) $^ -o $@

test_vmm: test_vmm.c
	$(CC) $(CFLAGS) $^ -o $@

run: all
	@for t in $(TESTS); do ./$$t || exit 1; done

clean:
	rm -f $(TESTS) *.o