# ---------- MSYS2 路径转换关闭 ----------
export MSYS_NO_PATHCONV := 1

# ---------- 目录 ----------
BUILD      := build
ESP        := $(BUILD)/esp
UEFI_SRC   := boot/uefi
KRN_SRC    := kernel
KRN_ARCH   := $(KRN_SRC)/arch/x64
SCRIPTS    := scripts

# ---------- 工具 ----------
CC         := clang
PYTHON     := python3

LD_UEFI    := $(shell command -v lld-link 2>/dev/null)
ifeq ($(LD_UEFI),)
$(error 找不到 lld-link，请安装：pacman -S mingw-w64-ucrt-x86_64-lld)
endif

LD_KRN     := $(LD_UEFI)     # 内核也用 lld-link（COFF），之后转 ELF

$(info [build] LD_UEFI = $(LD_UEFI))
$(info [build] LD_KRN  = $(LD_KRN) + $(PYTHON) $(SCRIPTS)/pe_to_elf.py)

# 与 boot.h 保持一致
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
# 关键：内核改用 COFF 目标（x86_64-pc-win32），用 lld-link 链接为 PE
KRN_CFLAGS  := $(FREESTAND) -target x86_64-pc-win32-coff \
               -mcmodel=kernel -I$(KRN_SRC) -I$(KRN_ARCH) \
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

# ---------- 目标 ----------
.PHONY: all clean

all: $(ESP)/EFI/BOOT/BOOTX64.EFI $(ESP)/kernel.elf

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

# 内核先由 lld-link 生成 PE32+
$(ESP)/kernel.pe: $(KRN_OBJS)
	@mkdir -p $(@D)
	$(LD_KRN) $(KRN_LDFLAGS) $(KRN_OBJS)

# 再由 Python 脚本把 PE32+ 转换为 ELF64 EXEC
$(ESP)/kernel.elf: $(ESP)/kernel.pe $(SCRIPTS)/pe_to_elf.py
	$(PYTHON) $(SCRIPTS)/pe_to_elf.py $< $@ \
	    $(KERNEL_LMA) $(KERNEL_VMA)

# ============================================================================
#  宿主侧单元测试（不经 UEFI / 内核工具链，直接在本机编译运行）
# ============================================================================

TEST_BUILD := $(BUILD)/test
HOST_CC    := cc
HOST_CFLAGS := -O1 -g -Ikernel -Ikernel/arch/x64 -Wall -Wextra
HOST_LDFLAGS :=

.PHONY: test test-kmalloc test-pmm test-vmm test-serial

test: test-kmalloc test-pmm test-vmm test-serial
	@echo "[test] all host tests passed"

test-kmalloc: $(TEST_BUILD)/test_kmalloc
	@$<

test-pmm: $(TEST_BUILD)/test_pmm
	@$<

test-vmm: $(TEST_BUILD)/test_vmm
	@$<

test-serial: $(TEST_BUILD)/test_serial
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

# test_serial 需要 kprintf.c 提供 ob_snprintf/ob_vsnprintf，
# 不需要真的串口实现（只调 ob_snprintf）
$(TEST_BUILD)/test_serial: $(KRN_SRC)/test_serial.c $(KPRINTF_C)
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) $^ -o $@

clean:
	rm -rf $(BUILD)