#OmniBridge/Makefile
export MSYS_NO_PATHCONV := 1

# ---------- 目录 ----------
BUILD      := build
ESP        := $(BUILD)/esp
UEFI_SRC   := boot/uefi
KRN_SRC    := kernel
KRN_ARCH   := $(KRN_SRC)/arch/x64
SCRIPTS    := scripts
USR        := usr

# ---------- 工具 ----------
CC         := clang
PYTHON     := python3

LD_UEFI    := $(shell command -v lld-link 2>/dev/null)
ifeq ($(LD_UEFI),)
$(error 找不到 lld-link，请安装：pacman -S mingw-w64-ucrt-x86_64-lld)
endif

LD_KRN     := $(LD_UEFI)

$(info [build] LD_UEFI = $(LD_UEFI))
$(info [build] LD_KRN  = $(LD_KRN) + $(PYTHON) $(SCRIPTS)/pe_to_elf.py)

KERNEL_VMA := 0xFFFFFFFF80000000
KERNEL_LMA := 0x00200000

# ---------- UEFI 侧：共享源文件探测 ----------
SERIAL_C    := $(firstword $(wildcard \
                 $(KRN_SRC)/serial.c \
                 $(KRN_SRC)/drivers/serial.c \
                 $(KRN_SRC)/lib/serial.c \
                 $(KRN_ARCH)/serial.c ))
KPRINTF_C   := $(firstword $(wildcard \
                 $(KRN_SRC)/kprintf.c \
                 $(KRN_SRC)/lib/kprintf.c \
                 $(KRN_ARCH)/kprintf.c ))

$(info [build] SERIAL_C  = $(SERIAL_C))
$(info [build] KPRINTF_C = $(KPRINTF_C))

ifeq ($(SERIAL_C),)
$(error 找不到 serial.c)
endif
ifeq ($(KPRINTF_C),)
$(error 找不到 kprintf.c)
endif

# ---------- 公共编译选项 ----------
FREESTAND  := -ffreestanding -fno-stack-protector -fno-builtin \
              -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -fno-pic \
              -fno-asynchronous-unwind-tables -fno-unwind-tables

# ---------- UEFI 侧 ----------
UEFI_CFLAGS  := $(FREESTAND) -target x86_64-pc-win32-coff \
                -I$(UEFI_SRC) -I$(KRN_SRC) -I$(KRN_ARCH) \
                -O2 -Wall -Wextra
UEFI_LDFLAGS := /subsystem:efi_application /entry:efi_main /nodefaultlib \
                /machine:x64 /out:$(ESP)/EFI/BOOT/BOOTX64.EFI

UEFI_OWN_C := main file elf page
UEFI_ASM   := io trampoline

UEFI_OBJS := \
    $(patsubst %,$(BUILD)/uefi/%.o,$(UEFI_OWN_C)) \
    $(BUILD)/uefi/serial.o \
    $(BUILD)/uefi/kprintf.o \
    $(patsubst %,$(BUILD)/uefi/%.o,$(UEFI_ASM))

# ---------- 内核侧 ----------
KRN_CFLAGS  := $(FREESTAND) -target x86_64-pc-win32-coff \
               -mcmodel=kernel -I$(KRN_SRC) -I$(KRN_ARCH) \
               -I$(KRN_ARCH)/net \
               -I$(KRN_ARCH)/block \
               -I$(KRN_ARCH)/user \
               -O2 -Wall -Wextra

KRN_LDFLAGS := /subsystem:native /entry:kernel_entry /nodefaultlib \
               /machine:x64 /base:$(KERNEL_VMA) \
               /out:$(ESP)/kernel.pe

KRN_ALL_C := $(shell find $(KRN_SRC) -name '*.c' \
                  ! -name 'test_*.c' ! -name 'stubs.c')
KRN_ALL_S := $(shell find $(KRN_ARCH) -name '*.S')

KRN_C_NAMES := $(notdir $(basename $(KRN_ALL_C)))
KRN_S_NAMES := $(notdir $(basename $(KRN_ALL_S)))

$(info [build] KRN C sources: $(KRN_C_NAMES))
$(info [build] KRN S sources: $(KRN_S_NAMES))

KRN_OBJS := \
    $(addprefix $(BUILD)/krn/,$(addsuffix .o,$(KRN_C_NAMES))) \
    $(addprefix $(BUILD)/krn/,$(addsuffix .o,$(KRN_S_NAMES)))

vpath %.c $(sort $(dir $(KRN_ALL_C)))
vpath %.S $(sort $(dir $(KRN_ALL_S)))

# ---------- 用户态库（oblibc） ----------
OBLIBC_DIR := $(USR)/lib/oblibc
OBLIBC_A   := $(OBLIBC_DIR)/liboblibc.a

# ============================================================================
# ★ 第 18C 步：内嵌用户程序到内核
#
# 流程：
#   usr/examples/<name>.c
#       --[obcc.sh]-->  build/esp/bin/<name>.obr
#       --[obr_to_c.py]--> build/gen/embedded_<name>.c
#       --[clang]-->  build/gen/embedded_<name>.o
#       --[lld-link]-->  kernel.pe 内嵌
#
# 由 main.c 通过 extern const 数组引用，并在根 FS 挂载后写入 VFS。
# ============================================================================

# 需要内嵌的程序列表（不带扩展名）
EMBEDDED_OBR_LIST := syscall_test hello fs_test exception_test \
                     signal_test pthread_test dyn_test libfoo \
                     pipe_test

EMBEDDED_OBRS  := $(addprefix $(BUILD)/esp/bin/,$(addsuffix .obr,$(EMBEDDED_OBR_LIST)))
EMBEDDED_GEN_C := $(addprefix $(BUILD)/gen/embedded_,$(addsuffix .c,$(EMBEDDED_OBR_LIST)))
EMBEDDED_GEN_O := $(EMBEDDED_GEN_C:.c=.o)

# 把内嵌目标文件追加到内核 OBJS
KRN_OBJS += $(EMBEDDED_GEN_O)

# 通用规则
$(BUILD)/esp/bin/%.obr: $(USR)/examples/%.c \
                        $(SCRIPTS)/obcc.sh \
                        $(SCRIPTS)/pe_to_obr.py \
                        $(USR)/include/ob/ob.h \
                        $(OBLIBC_A)
	@mkdir -p $(@D)
	bash $(SCRIPTS)/obcc.sh $< -o $@ --minpriv 0

# ★ 任务 3：dyn_test.obr 额外声明依赖 libfoo.obr
$(BUILD)/esp/bin/dyn_test.obr: $(USR)/examples/dyn_test.c \
                               $(SCRIPTS)/obcc.sh \
                               $(SCRIPTS)/pe_to_obr.py \
                               $(USR)/include/ob/ob.h \
                               $(OBLIBC_A) \
                               $(BUILD)/esp/lib/libfoo.obr
	@mkdir -p $(@D)
	bash $(SCRIPTS)/obcc.sh $< -o $@ --minpriv 0 --dep libfoo.obr

# ★ 任务 3：构建 libfoo.obr 到 /system/lib/ 路径
#   --no-libc：不链接 liboblibc.a（避免 crt0.o 引用 main）
$(BUILD)/esp/lib/libfoo.obr: $(USR)/examples/libfoo.c \
                             $(SCRIPTS)/obcc.sh \
                             $(SCRIPTS)/pe_to_obr.py \
                             $(USR)/include/ob/ob.h
	@mkdir -p $(@D)
	bash $(SCRIPTS)/obcc.sh $< -o $@ --minpriv 0 --no-libc

# 规则 2：.obr -> .c 数组
$(BUILD)/gen/embedded_%.c: $(BUILD)/esp/bin/%.obr \
                           $(SCRIPTS)/obr_to_c.py
	@mkdir -p $(@D)
	$(PYTHON) $(SCRIPTS)/obr_to_c.py $< $@ $*

# 规则 3：.c 数组 -> .o（使用内核编译选项）
$(BUILD)/gen/%.o: $(BUILD)/gen/%.c
	@mkdir -p $(@D)
	$(CC) $(KRN_CFLAGS) -c $< -o $@

# ---------- 目标 ----------
.PHONY: all clean oblibc test test-kmalloc test-pmm test-vmm test-serial \
        test-net-logic test-obfs-logic test-oblibc-logic test-pthread-logic

all: $(ESP)/EFI/BOOT/BOOTX64.EFI $(ESP)/kernel.elf $(OBLIBC_A) \
     $(EMBEDDED_OBRS) $(BUILD)/esp/lib/libfoo.obr
# ---- UEFI 编译规则 ----

$(BUILD)/uefi/%.o: $(UEFI_SRC)/%.c
	@mkdir -p $(@D)
	$(CC) $(UEFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/%.o: $(UEFI_SRC)/%.S
	@mkdir -p $(@D)
	$(CC) $(UEFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/serial.o: $(SERIAL_C)
	@mkdir -p $(@D)
	$(CC) $(UEFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/kprintf.o: $(KPRINTF_C)
	@mkdir -p $(@D)
	$(CC) $(UEFI_CFLAGS) -c $< -o $@

$(ESP)/EFI/BOOT/BOOTX64.EFI: $(UEFI_OBJS)
	@mkdir -p $(@D)
	$(LD_UEFI) $(UEFI_LDFLAGS) $(UEFI_OBJS)

# ---- 内核编译规则 ----

$(BUILD)/krn/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(KRN_CFLAGS) -c $< -o $@

$(BUILD)/krn/%.o: %.S
	@mkdir -p $(@D)
	$(CC) $(KRN_CFLAGS) -c $< -o $@

# 内核 PE32+ 链接（含内嵌 .obr 目标文件）
$(ESP)/kernel.pe: $(KRN_OBJS)
	@mkdir -p $(@D)
	$(LD_KRN) $(KRN_LDFLAGS) $(KRN_OBJS)

# PE32+ -> ELF64 EXEC
$(ESP)/kernel.elf: $(ESP)/kernel.pe $(SCRIPTS)/pe_to_elf.py
	$(PYTHON) $(SCRIPTS)/pe_to_elf.py $< $@ \
	    $(KERNEL_LMA) $(KERNEL_VMA)

# ---- 用户态库 oblibc ----
oblibc: $(OBLIBC_A)

$(OBLIBC_A):
	$(MAKE) -C $(OBLIBC_DIR)

# ============================================================================
#  宿主侧单元测试
# ============================================================================

TEST_BUILD := $(BUILD)/test
HOST_CC    := cc
HOST_CFLAGS := -O1 -g -Ikernel -Ikernel/arch/x64 -Ikernel/arch/x64/net \
               -Ikernel/arch/x64/block -Ikernel/arch/x64/user \
               -Wall -Wextra
HOST_LDFLAGS :=

test: test-kmalloc test-pmm test-vmm test-serial test-net-logic \
      test-obfs-logic test-oblibc-logic test-pthread-logic
	@echo "[test] all host tests passed"

test-kmalloc: $(TEST_BUILD)/test_kmalloc
	@$<

test-pmm: $(TEST_BUILD)/test_pmm
	@$<

test-vmm: $(TEST_BUILD)/test_vmm
	@$<

test-serial: $(TEST_BUILD)/test_serial
	@$<

test-net-logic: $(TEST_BUILD)/test_net_logic
	@$<

test-obfs-logic: $(TEST_BUILD)/test_obfs_logic
	@$<

test-oblibc-logic: $(TEST_BUILD)/test_oblibc_logic
	@$<

test-pthread-logic: $(TEST_BUILD)/test_pthread_logic
	@$<

$(TEST_BUILD)/test_kmalloc: $(KRN_SRC)/test_kmalloc.c
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) $< -o $@

$(TEST_BUILD)/test_pmm: $(KRN_SRC)/test_pmm.c
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) $< -o $@

$(TEST_BUILD)/test_vmm: $(KRN_SRC)/test_vmm.c
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) $< -o $@

$(TEST_BUILD)/test_serial: $(KRN_SRC)/test_serial.c $(KPRINTF_C)
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) $^ -o $@

$(TEST_BUILD)/test_net_logic: tests/host/test_net_logic.c
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) $< -o $@

$(TEST_BUILD)/test_obfs_logic: tests/host/test_obfs_logic.c
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) $< -o $@

$(TEST_BUILD)/test_oblibc_logic: tests/host/test_oblibc_logic.c
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) $< -o $@

$(TEST_BUILD)/test_pthread_logic: tests/host/test_pthread_logic.c
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) $< -lpthread -o $@

# ============================================================================
#  清理
# ============================================================================

clean:
	rm -rf $(BUILD)
	$(MAKE) -C $(OBLIBC_DIR) clean