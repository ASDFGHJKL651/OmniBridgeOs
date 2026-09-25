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

# ============================================================================
# ★ 第 19 步：Linux ELF64 静态 hello —— 使用 clang 直出
#
# 关键点（人工必须审查）：
#   - clang 通过 -target 切换目标三元组即可产出 ELF64（而非默认 COFF）；
#   - 链接必须强制使用 lld（ELF 版）：-fuse-ld=lld；
#     MSYS2 的 clang 若自动选用 lld-link（COFF 版），会失败；
#   - -nostdlib -static -no-pie：不依赖 libc，不生成 PT_INTERP；
#   - -fno-stack-protector -fno-pic：避免对 __stack_chk_fail / GOT 的
#     未定义引用；
#   - linux_hello.c 使用 _start + 内联汇编直接发 syscall，无需 main；
#   - 输出是真正的 Linux ELF64 ET_EXEC；readelf -h 可验证。
#
# 关键修复历史：
#   v1：把 -no-pie 放在编译期 → clang 报 unused 警告；ELF 实际是 ET_DYN；
#       且默认基址 0x200000 与内核 PML4[0] 的 2MB HUGE 恒等映射冲突。
#   v2：改用 -Wl,-Ttext-segment=0x8000000000 → ld.lld 不识别该选项。
#   v3（本版本）：
#     - 用 -Wl,--image-base=0x8000000000 替代 GNU ld 的 -Ttext-segment。
#     - -fno-pie（编译期）+ -Wl,--no-pie（链接期）双管齐下，确保 ET_EXEC。
#     - -Wl,-e,_start 显式指定入口符号。
#     - ★ 关键：-mcmodel=large（默认 small 会生成 R_X86_64_32 重定位，
#       超出 32 位范围时 ld.lld 报 "relocation out of range"）。
# ============================================================================

LINUX_HELLO_SRC := $(USR)/examples/linux_hello.c
LINUX_HELLO_OUT := $(BUILD)/esp/bin/linux_hello.elf

LX_TARGET   := x86_64-linux-gnu
LX_BASE     := 0x8000000000

LX_CFLAGS   := --target=$(LX_TARGET) \
               -nostdlib -static \
               -fno-pic -fno-pie \
               -fno-stack-protector -fno-builtin \
               -mno-red-zone \
               -mcmodel=large \
               -fno-asynchronous-unwind-tables -fno-unwind-tables \
               -O2 -Wall -Wextra

LX_LDFLAGS  := -fuse-ld=lld \
               -Wl,--no-pie \
               -Wl,--image-base=$(LX_BASE) \
               -Wl,-e,_start

LLD_ELF     := $(shell command -v ld.lld 2>/dev/null)

$(LINUX_HELLO_OUT): $(LINUX_HELLO_SRC)
	@mkdir -p $(@D)
	@if [ -z "$(LLD_ELF)" ]; then \
	    echo "[lxcc] WARN: ld.lld not found; skip $@"; \
	    echo "[lxcc]       install: pacman -S mingw-w64-ucrt-x86_64-lld"; \
	else \
	    echo "[lxcc] $(CC) --target=$(LX_TARGET) -nostdlib @ $(LX_BASE) -> $@"; \
	    MSYS_NO_PATHCONV=1 $(CC) $(LX_CFLAGS) $(LX_LDFLAGS) -o $@ $< ; \
	fi

.PHONY: linux-hello
linux-hello: $(LINUX_HELLO_OUT)

# ★ linux_hello.elf -> 内嵌到内核
LINUX_HELLO_EMBED_C := $(BUILD)/gen/embedded_linux_hello.c
LINUX_HELLO_EMBED_O := $(BUILD)/gen/embedded_linux_hello.o

KRN_OBJS += $(LINUX_HELLO_EMBED_O)

$(LINUX_HELLO_EMBED_C): $(LINUX_HELLO_OUT) $(SCRIPTS)/obr_to_c.py
	@mkdir -p $(@D)
	@if [ -f "$<" ]; then \
	    $(PYTHON) $(SCRIPTS)/obr_to_c.py $< $@ linux_hello ; \
	else \
	    echo "[embed] WARN: $< absent, generating empty stub"; \
	    printf '/* auto: linux_hello.elf absent */\n#include <stdint.h>\n' \
	        > $@ ; \
	    printf 'const uint8_t g_embedded_linux_hello[] = {0};\n' >> $@ ; \
	    printf 'const uint64_t g_embedded_linux_hello_len = 0ull;\n' >> $@ ; \
	fi

$(LINUX_HELLO_EMBED_O): $(LINUX_HELLO_EMBED_C)
	@mkdir -p $(@D)
	$(CC) $(KRN_CFLAGS) -c $< -o $@

# ============================================================================
# ★ 第 20 步：Win32 PE 子集
#
# 关键点（人工必须审查）：
#   - WIN32_HELLO_EMBED_O 必须先 := 定义，KRN_OBJS += 才能生效。
#     本整块必须位于 $(ESP)/kernel.pe: 规则之前（Makefile 顺序解析）。
#   - --image-base,0x8000000000 = 512 GiB。这是修复 "用户态 #PF(I=1)"
#     的关键：内核用户 PML4[0] 复制了恒等映射 0..4GB（2MB 大页，U=0），
#     任何 ImageBase 落在 PML4[0] 都会被 CPL=3 拒绝。
#     0x8000000000 的 PML4 索引 = 1，位于用户 PML4[1..255] 范围内。
#   - -Wl,-e,mainCRTStartup_win32：显式入口符号。
#   - -Wl,--subsystem,console：子系统 = Console (3)。
#   - -nostdlib：不链接 CRT。但必须显式 -lkernel32 以解析
#     __imp_GetStdHandle / __imp_WriteFile / __imp_ExitProcess 符号。
#     注意：-nostdlib 只禁止"自动"链接默认库；-lkernel32 是显式指定，
#     不受 -nostdlib 影响。
# ============================================================================
WIN32_HELLO_SRC := $(USR)/examples/win32_hello.c
WIN32_HELLO_OUT := $(BUILD)/esp/bin/win32_hello.exe

WIN32_CC     := $(shell command -v x86_64-w64-mingw32-gcc 2>/dev/null)
ifeq ($(WIN32_CC),)
WIN32_CC     := $(shell command -v clang 2>/dev/null)
WIN32_CC_MODE := clang-mingw
endif

WIN32_HELLO_EMBED_C := $(BUILD)/gen/embedded_win32_hello.c
WIN32_HELLO_EMBED_O := $(BUILD)/gen/embedded_win32_hello.o

# ★★ 关键行：此刻 WIN32_HELLO_EMBED_O 已定义，追加才有效 ★★
KRN_OBJS += $(WIN32_HELLO_EMBED_O)

$(WIN32_HELLO_OUT): $(WIN32_HELLO_SRC)
	@mkdir -p $(@D)
	@if [ -z "$(WIN32_CC)" ]; then \
	    echo "[win32cc] WARN: no mingw-gcc / clang; skipping $@"; \
	    printf 'not-built\n' > $@ ; \
	elif [ "$(WIN32_CC_MODE)" = "clang-mingw" ]; then \
	    MSYS_NO_PATHCONV=1 $(WIN32_CC) \
	        --target=x86_64-pc-windows-gnu \
	        -nostdlib -ffreestanding -O2 \
	        -Wl,-e,mainCRTStartup_win32 \
	        -Wl,--subsystem,console \
	        -Wl,--image-base,0x8000000000 \
	        -o $@ $< -lkernel32 ; \
	else \
	    MSYS_NO_PATHCONV=1 $(WIN32_CC) \
	        -nostdlib -ffreestanding -O2 \
	        -Wl,-e,mainCRTStartup_win32 \
	        -Wl,--subsystem,console \
	        -Wl,--image-base,0x8000000000 \
	        -o $@ $< -lkernel32 ; \
	fi

$(WIN32_HELLO_EMBED_C): $(WIN32_HELLO_OUT) $(SCRIPTS)/obr_to_c.py
	@mkdir -p $(@D)
	@if [ -f "$<" ] && [ -s "$<" ] && ! head -c 1 "$<" | grep -q '^n$$'; then \
	    $(PYTHON) $(SCRIPTS)/obr_to_c.py $< $@ win32_hello ; \
	else \
	    echo "[embed] WARN: win32_hello.exe absent, stub"; \
	    printf '/* auto: win32_hello.exe absent */\n' > $@ ; \
	    printf '#include <stdint.h>\n' >> $@ ; \
	    printf 'const uint8_t g_embedded_win32_hello[] = {0};\n' >> $@ ; \
	    printf 'const uint64_t g_embedded_win32_hello_len = 0ull;\n' >> $@ ; \
	fi

$(WIN32_HELLO_EMBED_O): $(WIN32_HELLO_EMBED_C)
	@mkdir -p $(@D)
	$(CC) $(KRN_CFLAGS) -c $< -o $@

.PHONY: win32-hello
win32-hello: $(WIN32_HELLO_OUT)

# ★ 第 20 步扩展：Win32 API 综合测试
WIN32_API_TEST_SRC := $(USR)/examples/win32_api_test.c
WIN32_API_TEST_OUT := $(BUILD)/esp/bin/win32_api_test.exe

WIN32_API_TEST_EMBED_C := $(BUILD)/gen/embedded_win32_api_test.c
WIN32_API_TEST_EMBED_O := $(BUILD)/gen/embedded_win32_api_test.o

KRN_OBJS += $(WIN32_API_TEST_EMBED_O)

$(WIN32_API_TEST_OUT): $(WIN32_API_TEST_SRC)
	@mkdir -p $(@D)
	@if [ -z "$(WIN32_CC)" ]; then \
	    echo "[win32cc] WARN: no mingw-gcc / clang; skipping $@"; \
	    printf 'not-built\n' > $@ ; \
	elif [ "$(WIN32_CC_MODE)" = "clang-mingw" ]; then \
	    MSYS_NO_PATHCONV=1 $(WIN32_CC) \
	        --target=x86_64-pc-windows-gnu \
	        -nostdlib -ffreestanding -O2 \
	        -Wl,-e,mainCRTStartup_win32 \
	        -Wl,--subsystem,console \
	        -Wl,--image-base,0x8000000000 \
	        -o $@ $< -lkernel32 ; \
	else \
	    MSYS_NO_PATHCONV=1 $(WIN32_CC) \
	        -nostdlib -ffreestanding -O2 \
	        -Wl,-e,mainCRTStartup_win32 \
	        -Wl,--subsystem,console \
	        -Wl,--image-base,0x8000000000 \
	        -o $@ $< -lkernel32 ; \
	fi

$(WIN32_API_TEST_EMBED_C): $(WIN32_API_TEST_OUT) $(SCRIPTS)/obr_to_c.py
	@mkdir -p $(@D)
	@if [ -f "$<" ] && [ -s "$<" ] && ! head -c 1 "$<" | grep -q '^n$$'; then \
	    $(PYTHON) $(SCRIPTS)/obr_to_c.py $< $@ win32_api_test ; \
	else \
	    echo "[embed] WARN: win32_api_test.exe absent, stub"; \
	    printf '/* auto: win32_api_test.exe absent */\n' > $@ ; \
	    printf '#include <stdint.h>\n' >> $@ ; \
	    printf 'const uint8_t g_embedded_win32_api_test[] = {0};\n' >> $@ ; \
	    printf 'const uint64_t g_embedded_win32_api_test_len = 0ull;\n' >> $@ ; \
	fi

$(WIN32_API_TEST_EMBED_O): $(WIN32_API_TEST_EMBED_C)
	@mkdir -p $(@D)
	$(CC) $(KRN_CFLAGS) -c $< -o $@

.PHONY: win32-api-test
win32-api-test: $(WIN32_API_TEST_OUT)

# ---------- 目标 ----------
.PHONY: all clean oblibc test test-kmalloc test-pmm test-vmm test-serial \
        test-net-logic test-obfs-logic test-oblibc-logic test-pthread-logic \
        linux-hello win32-hello

all: $(ESP)/EFI/BOOT/BOOTX64.EFI $(ESP)/kernel.elf $(OBLIBC_A) \
     $(EMBEDDED_OBRS) $(BUILD)/esp/lib/libfoo.obr \
     $(LINUX_HELLO_OUT) $(WIN32_HELLO_OUT) $(WIN32_API_TEST_OUT)

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

# 内核 PE32+ 链接（含内嵌 .obr / .elf / .exe 目标文件）
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