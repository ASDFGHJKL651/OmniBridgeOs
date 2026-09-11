===============================================================================
                      OmniBridge OS 开发进度 README
                        内核代号：Knot
                        用户态代号：Tide
                        当前阶段：UEFI 引导与内核基础已打通，
                        正在开发第 4 步（GDT/TSS/IDT/异常/系统调用门）
===============================================================================

【文档说明】
本文档描述 OmniBridge OS 当前开发进度、构建方式、运行方式、已实现功能、
待完成事项与已知问题。当前仓库处于早期开发阶段，目标是先打通 UEFI 引导、
x64 长模式、内核入口、串口日志、GDT/IDT、物理内存、虚拟内存与 SLAB 分配器，
再逐步补齐 TSS、IRQ、定时器、系统调用门与权限引擎。

===============================================================================
1. 项目简介
===============================================================================

OmniBridge OS 是一个从零构建、面向 x64 架构、以 C 语言为主的独立操作系统。
设计目标是通过“内核极简稳固，用户态承载安全，兼容层实现生态，权限驱动信任”
的方式，实现多生态兼容与强权限隔离。

当前已完成：

  - UEFI BootLoader
  - x64 长模式切换
  - 初始页表建立
  - 内核 ELF 加载
  - 串口输出
  - GDT / IDT 基础
  - 物理内存管理（PMM）
  - 虚拟内存管理（VMM）
  - 内核 SLAB 分配器（kmalloc）
  - QEMU + OVMF 自动运行与串口日志校验

===============================================================================
2. 当前进度概览
===============================================================================

根据《OmniBridge OS 完整开发步骤文档》：

  第 1 步：范围冻结与需求追踪
    状态：文档基本具备，工程交付物待补齐
    说明：已有技术规格与开发规划

  第 2 步：工具链与 CI
    状态：基本完成
    说明：构建、QEMU 启动、串口日志链路已通

  第 3 步：UEFI BootLoader 与长模式
    状态：基本完成
    说明：成功读取 kernel.elf，建立页表，跳转内核

  第 4 步：GDT/TSS/IDT/异常/系统调用门
    状态：进行中，约 30%~40%
    说明：GDT、IDT 异常桩、串口已完成；
          TSS、IRQ、APIC、系统调用门未完成

  第 5/6 步：物理内存、虚拟内存、SLAB
    状态：部分超前实现
    说明：PMM、VMM、kmalloc 已可运行

当前串口日志已可输出：

  Knot booting
  OmniBridge OS UEFI boot stub v0.3
  Kernel codename: Knot, userspace codename: Tide
  [UEFI] kernel.elf loaded, 20480 bytes
  [UEFI] ELF entry=0xffffffff80003e20 LMA=0x200000
  [UEFI] page tables built
  [UEFI] Boot Services exited
  [UEFI] handoff to kernel at 0xffffffff80003e20, entries=94
  [GDT] loaded
  [IDT] loaded
  [PMM] usable pages: 128960, free: 128960
  [VMM] 4-level paging enabled
  [KMALLOC] ready, 9 size classes
  Hello Kernel
  kmalloc OK

===============================================================================
3. 目录结构
===============================================================================

OmniBridgeOs/
├── boot/
│   └── uefi/
│       ├── main.c            # UEFI 入口，读取 kernel.elf，退出 BootServices
│       ├── file.c / file.h   # FAT32 文件读取
│       ├── elf.c / elf.h     # ELF64 解析与加载
│       ├── page.c / page.h   # UEFI 阶段初始页表
│       ├── trampoline.S      # 跳转内核入口
│       ├── io.S              # 串口 I/O 封装
│       └── uefi_min.h        # 最小 UEFI 类型与协议定义
├── kernel/
│   ├── arch/x64/
│   │   ├── entry.S           # 异常 stub、GDT/IDT 加载
│   │   ├── entry_kernel.S    # 内核入口，切换内核栈
│   │   ├── gdt.c / gdt.h     # GDT 初始化
│   │   ├── idt.c / idt.h     # IDT 初始化与异常处理
│   │   ├── serial.c / serial.h
│   │   ├── printk.c / printk.h
│   │   └── boot.h            # 内核 LMA/VMA 与 boot_info
│   ├── mm/
│   │   ├── pmm.c / pmm.h     # 伙伴系统物理内存管理
│   │   ├── vmm.c / vmm.h     # 4 级页表与内核高半区映射
│   │   └── kmalloc.c / kmalloc.h # SLAB 分配器
│   └── main.c                # _kstart_c 内核 C 入口
├── scripts/
│   ├── build.ps1
│   ├── run-qemu.ps1
│   ├── run-qemu.sh
│   ├── setup-toolchain.ps1
│   ├── setup-toolchain.sh
│   └── check-serial.ps1
├── tests/
│   ├── test_serial.c
│   ├── test_vmm.c
│   ├── test_pmm.c
│   └── test_kmalloc.c
├── tools/
│   └── pe_to_elf.py          # PE32+ -> ELF64 转换工具
├── build/                    # 构建输出
│   ├── esp/EFI/BOOT/BOOTX64.EFI
│   └── serial.log
└── README.txt

注：目录结构根据当前文件推断，实际可能略有差异。

===============================================================================
4. 构建与运行环境
===============================================================================

4.1 必需工具

  - MSYS2（UCRT64 或 MINGW64）
  - clang / LLVM
  - lld-link
  - nasm
  - binutils
  - make / cmake / ninja
  - QEMU for Windows
  - OVMF 固件（需手动下载，见 4.3 节）

4.2 安装工具链

在 PowerShell 中运行：

  .\scripts\setup-toolchain.ps1

或直接在 MSYS2 中运行：

  ./scripts/setup-toolchain.sh

脚本会检查并安装：

  - mingw-w64-ucrt-x86_64-clang
  - mingw-w64-ucrt-x86_64-lld
  - mingw-w64-ucrt-x86_64-binutils
  - mingw-w64-ucrt-x86_64-nasm
  - mingw-w64-ucrt-x86_64-gdb
  - mingw-w64-ucrt-x86_64-qemu

4.3 OVMF 获取（必须手动下载）

注意：
  MSYS2 仓库中通常没有 edk2-ovmf 包，OVMF 固件必须手动下载。
  请在 MSYS2 UCRT64 Shell 中执行，不要使用 PowerShell 或 CMD。

步骤：

  1) 打开 MSYS2 UCRT64 Shell。

  2) 进入项目根目录，例如：

       cd /d/OmniBridgeOs

     如果项目在其他盘符，请按实际路径调整。

  3) 下载 OVMF：

       curl -L -o OVMF.fd https://github.com/retrage/edk2-nightly/raw/master/bin/RELEASEX64_OVMF.fd

  4) 设置 OVMF_PATH：

       export OVMF_PATH=$(pwd)/OVMF.fd

  5) 验证文件存在：

       ls -l "$OVMF_PATH"

  6) 运行 QEMU：

       ./scripts/run-qemu.sh

说明：
  - export OVMF_PATH=$(pwd)/OVMF.fd 只对当前 MSYS2 UCRT64 Shell 会话有效。
  - 新开一个 Shell 后，需要重新执行 export，或写入 ~/.bashrc。
  - 如果使用 PowerShell 的 run-qemu.ps1，需要设置 Windows 路径，例如：

      $env:OVMF_PATH = "D:\OmniBridgeOs\OVMF.fd"

  - run-qemu.sh 会自动把 MSYS 路径转换为 Windows 路径。
  - 如果 curl 不存在，可先安装：

      pacman -S curl

===============================================================================
5. 快速开始
===============================================================================

5.1 构建

在 PowerShell 中：

  .\scripts\build.ps1

该脚本会调用 MSYS2 中的 make all，最终生成：

  build/esp/EFI/BOOT/BOOTX64.EFI

5.2 运行 QEMU

方式一：PowerShell

  .\scripts\run-qemu.ps1

方式二：MSYS2 UCRT64 Shell

  export OVMF_PATH=$(pwd)/OVMF.fd
  ./scripts/run-qemu.sh

脚本会自动：

  - 查找 QEMU 可执行文件
  - 查找 OVMF 固件
  - 创建 FAT 磁盘映像
  - 启动 QEMU
  - 将串口输出保存到 build/serial.log

5.3 校验串口日志

  .\scripts\check-serial.ps1

当前脚本要求日志中包含以下关键内容：

  - Knot booting
  - OmniBridge OS UEFI boot stub v0.3
  - Kernel codename: Knot, userspace codename: Tide
  - [UEFI] kernel.elf loaded
  - [UEFI] ELF entry=
  - [UEFI] page tables built
  - [UEFI] Boot Services exited
  - [UEFI] handoff to kernel at
  - [KRN] Hello Kernel
  - [GDT] loaded
  - [IDT] loaded
  - [PMM] usable pages:
  - [VMM] 4-level paging enabled
  - Hello Kernel
  - kmalloc OK

注意：
  实际日志中输出为 Hello Kernel，而脚本要求 [KRN] Hello Kernel，
  存在前缀不一致，后续需统一。

===============================================================================
6. 已实现功能
===============================================================================

6.1 UEFI BootLoader

  - 从 FAT32 分区读取 kernel.elf
  - 解析 ELF64，按 p_paddr 加载 PT_LOAD 段
  - 清零 BSS
  - 获取 UEFI 内存图
  - 建立初始页表：
      * 恒等映射 0..4GB（过渡用）
      * DirectMap 0xFFFF800000000000 + phys
      * 内核高半区 0xFFFFFFFF80000000 起 1GB
  - 退出 BootServices
  - 跳转内核入口 _kstart_c

6.2 内核基础

  - GDT：内核代码段、内核数据段、用户段
  - IDT：0~31 异常门
  - 串口驱动：COM1，轮询输出
  - printk / serial_printf
  - 异常处理桩：除零、页错误等可打印并停机

6.3 内存管理

  - PMM：伙伴系统，4K 页帧，alloc_pages/free_pages
  - VMM：4 级页表，内核高半区映射，CR3 加载
  - kmalloc：9 个大小类，SLAB 风格，支持 kmalloc/kzalloc/kfree

6.4 工具与测试

  - pe_to_elf.py：PE32+ 转 ELF64
  - 宿主侧单元测试：
      * test_serial.c
      * test_vmm.c
      * test_pmm.c
      * test_kmalloc.c

===============================================================================
7. 待完成 / 下一步计划
===============================================================================

第 4 步尚未完成，主要缺口：

  1. TSS 初始化
     - 定义 TSS 结构
     - GDT 增加 TSS 描述符
     - 实现 tss_init() 与 ltr
     - 设置 RSP0 指向内核栈

  2. IRQ 与中断控制器
     - 重映射 PIC 或初始化 LAPIC/IOAPIC
     - IDT 注册 32~47 IRQ 入口
     - 实现定时器（PIT 或 APIC timer）
     - 验证定时器中断可捕获

  3. 系统调用门
     - 配置 EFER.SCE
     - 配置 STAR、LSTAR、FMASK
     - 编写 asm_syscall_entry 汇编入口
     - 实现 C 桩 syscall_dispatcher 骨架

  4. 异常处理可恢复化
     - 区分内核异常与用户异常
     - 除零/页错误测试不应直接 qemu_exit
     - 预留 do_exit(SIGKILL) 路径

  5. CI 与日志统一
     - 修正 check-serial.ps1 中 [KRN] Hello Kernel 与实际输出不一致
     - 增加除零、页错误、定时器、syscall 测试

  6. 地址布局确认
     - 规格书写内核高地址为 0xFFFF800000000000
     - 当前实现为 0xFFFFFFFF80000000
     - 需确认是否修正规格或调整链接脚本，并记录到需求追踪矩阵

===============================================================================
8. 已知问题与注意事项
===============================================================================

  问题：异常处理不可恢复
    说明：当前异常统一 panic / qemu_exit
    建议：后续改为终止当前任务，保留内核运行

  问题：无 TSS
    说明：用户态到内核态栈切换缺失
    建议：第 4 步必须补齐

  问题：无 IRQ / 定时器
    说明：调度器无法获得时钟节拍
    建议：第 4 步补齐 PIC/APIC 与 PIT

  问题：无系统调用门
    说明：权限引擎无入口
    建议：第 4 步补齐 syscall MSR 与汇编入口

  问题：日志前缀不一致
    说明：check-serial.ps1 要求 [KRN] Hello Kernel，实际为 Hello Kernel
    建议：统一日志格式

  问题：内核 VMA 偏差
    说明：OB_KERNEL_VMA = 0xFFFFFFFF80000000，规格书写 0xFFFF800000000000
    建议：确认后更新文档或代码

  问题：第 5/6 步超前
    说明：PMM/VMM/kmalloc 已部分实现
    建议：先冻结第 4 步最小闭环

===============================================================================
9. 测试
===============================================================================

宿主侧测试可直接编译运行，例如：

  gcc -I kernel/arch/x64 tests/test_serial.c kernel/arch/x64/kprintf.c -o test_serial
  ./test_serial

其他测试类似。

QEMU 内测试通过串口日志校验脚本进行：

  .\scripts\check-serial.ps1

===============================================================================
10. 参考文档
===============================================================================

  - OmniBridgeOS开发规划.txt
  - OmniBridgeOs系统开发要求.txt
  - elf.c / elf.h
  - file.c / file.h
  - main.c（UEFI）
  - page.c / page.h
  - trampoline.S
  - entry.S / entry_kernel.S
  - gdt.c / idt.c
  - pmm.c / vmm.c / kmalloc.c
  - build.ps1 / run-qemu.ps1 / check-serial.ps1
  - setup-toolchain.sh
  - serial.log

===============================================================================
11. 贡献与审查原则
===============================================================================

AI 可生成：

  - 构建脚本、CI、日志、CLI、解析器、测试、桩、API 映射表、文档、
    样板、缓存、UI 样板、包管理。

人工必须深度审查：

  - 上下文切换、页表、锁、中断、权限引擎、PID 保护、ITA 签名、SEE 路径、
    EPT/VMExit、IOMMU、DMA、审计、逃逸检测、安全不变量。

关键核心不能“简单审核”：

  - AI 只生成框架/伪代码，人工重写或逐行验证。

===============================================================================
最终结论
===============================================================================

当前进度：

  - 第 3 步基本完成。
  - 第 4 步进行中，约 30%~40%。
  - 第 5/6 步部分超前实现。

当前目标：

  完成第 4 步最小闭环，进入第 5 步物理内存与 SLAB 正式验收。

===============================================================================
文档结束
===============================================================================