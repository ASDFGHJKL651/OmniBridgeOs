#===OmniBridgeOs/usr/lib/oblibc/Makefile===#
# 生成用户态静态库 liboblibc.a（COFF 目标）

CC      := clang

# ★ 修复：优先 llvm-ar（与 COFF 目标兼容性最好）；若不可用则回退到
#   GNU ar（binutils 提供）。若两者都找不到，立即报错，而不是让
#   make 在链接阶段静默失败。
#
#   注意：`clang` 与 `llvm-ar` 分属不同的 MSYS2 包：
#     - mingw-w64-ucrt-x86_64-clang   （提供 clang）
#     - mingw-w64-ucrt-x86_64-llvm    （提供 llvm-ar/llvm-objcopy）
#   只安装 clang 不会带上 llvm-ar。
AR := $(shell command -v llvm-ar 2>/dev/null)
ifeq ($(AR),)
AR := $(shell command -v ar 2>/dev/null)
endif
ifeq ($(AR),)
$(error 未找到 llvm-ar 或 ar。请安装：pacman -S mingw-w64-ucrt-x86_64-llvm)
endif

$(info [oblibc] CC = $(shell command -v $(CC) 2>/dev/null || echo $(CC)))
$(info [oblibc] AR = $(AR))

CFLAGS  := -target x86_64-pc-win32-coff \
           -ffreestanding -fno-stack-protector -fno-builtin \
           -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
           -fno-pic -fno-asynchronous-unwind-tables -fno-unwind-tables \
           -mcmodel=small -O2 -Wall -Wextra \
           -I../../include

SRCS_C := string.c stdlib.c stdio.c syscall.c pthread.c signal.c time.c
OBJS_C := $(SRCS_C:.c=.o)
OBJS_S := crt0.o
OBJS   := $(OBJS_C) $(OBJS_S)

.PHONY: all clean

all: liboblibc.a

liboblibc.a: $(OBJS)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o liboblibc.a
#===OmniBridgeOs/usr/lib/oblibc/Makefile 结束===#