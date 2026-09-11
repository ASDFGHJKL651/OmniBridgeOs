===============================================================================
                      OmniBridge OS 完整系统 README
                        内核代号：Knot
                        用户态代号：Tide
                        技术规格：v5.0
===============================================================================

【文档说明】
本文档为 OmniBridge OS 操作系统的完整 README，涵盖系统定位、架构设计、
核心特性、权限体系、信任模型、沙盒机制、兼容层、文件系统、启动流程、
开发路线图、构建运行方式、目录结构、测试与贡献原则。
适用于开发者、审查者与系统集成人员。

===============================================================================
1. 项目名称与标识
===============================================================================

系统名称：OmniBridge OS
版本号：v1.0（初始开发目标）
内核代号：Knot（结，象征稳固与连接）
用户态代号：Tide（潮，象征流动与兼容）

命名释义：
  - Omni（全、遍在）体现其对多架构（x64/x86/ARM）、多生态
    （Windows/Linux/自研）的兼容野心。
  - Bridge（桥接）精准概括其核心设计哲学——不是替代，而是连接与转化，
    使异构应用能在统一内核上平等运行。

===============================================================================
2. 项目总体定位
===============================================================================

OmniBridge OS 是一个从零构建、面向 x64 架构、以 C 语言为主
（C++ 用于部分用户态工具）的独立操作系统。其核心设计原则为：

    “内核极简稳固，用户态承载安全，兼容层实现生态，权限驱动信任。”

系统不以“完整复刻”现有系统为目标，而是通过行为仿真 + 请求重映射的方式，
在最小内核支持下实现最大应用兼容性。所有兼容程序与沙盒程序均受原生 9 级
权限体系严格约束，无特权逃逸窗口。

===============================================================================
3. 架构总体分层
===============================================================================

+------------------+-----------------------------------+----------+--------------+
| 层级             | 组件                              | 开发语言 | 权限级别     |
+------------------+-----------------------------------+----------+--------------+
| 硬件层           | 中断/内存/IO管理、CPU调度          | C+汇编   | 最高（内核态）|
| 内核核心         | 进程管理、IPC、VFS、权限检查引擎、  | C        | 最高         |
|                  | 沙盒执行引擎（SEE）                |          |              |
| 内核扩展         | 驱动框架、内核态沙盒（K-Sandbox）、 | C        | 最高         |
|                  | 安全监控器、审计日志                |          |              |
| 系统服务层       | 进程启动器（Init）、策略配置接口    | C/C++    | 最高/极高    |
|                  | （SecMgr）、审计读取（Auditd）      |          |              |
| 兼容运行时       | Win32/Linux/ARM仿真层               | C/C++    | 高～极高     |
|                  | （驻留于 Init 进程）                |          |              |
| 用户态核心       | Shell（OShell）、UI服务、包管理、   | C/C++    | 高～最高     |
|                  | 编译器（OCompiler）                 |          |              |
| 用户应用         | 普通应用、脚本、第三方程序          | 任意     | 最低～较高   |
+------------------+-----------------------------------+----------+--------------+

权限检查引擎直接集成于内核系统调用入口，所有资源访问均先验证进程安全令牌，
确保权限判定原子性且不可绕过。内核内存及内核文件目录（/kernel/、
/system/kernel/）在任何用户态进程的访问请求中均被无条件拒绝，无论该进程
权限等级如何。

===============================================================================
4. 核心特性
===============================================================================

4.1 九级权限体系

+------+----------------+--------------------------------------------------+
| 等级 | 名称           | 核心限制                                          |
+------+----------------+--------------------------------------------------+
| 0    | 最低           | 进程隔离：仅访问自身内存和内存 VFS；禁止任何外部   |
|      |                | 资源；无子进程；无 IPC                             |
| 1    | 极低           | 进程隔离：仅访问自身内存和专用临时目录；子进程≤1； |
|      |                | CPU≤5%                                            |
| 2    | 低             | 访问低完整度目录，禁止执行文件（可访问部分共享）   |
| 3    | 较低           | 部分目录读写，子进程继承低权限，提权需 UAC         |
| 4    | 中             | 类似 Windows 标准用户，可安装部分应用              |
| 5    | 较高           | 访问除系统目录外所有目录                           |
| 6    | 高             | 修改非安全系统配置（需 UAC 确认）                  |
| 7    | 极高           | 只读全盘，读取其他进程内存（不可写）               |
| 8    | 最高           | 全权操作（仅系统信任程序/服务）                    |
| 9    | 系统管理员     | 拥有最高硬件与内核数据访问权限（可读写内核内存段、 |
|      | （内核管理器） | 访问 /kernel/ 及 /system/kernel/ 目录），但必须    |
|      | 专属）         | 通过 UI 交互上下文发起所有敏感操作，且进程必须持有 |
|      |                | 内核颁发的 UI 会话令牌；该等级不可被任何子进程     |
|      |                | 继承，仅由内核在启动时通过指定路径创建。            |
+------+----------------+--------------------------------------------------+

特权继承规则：
  - 子进程默认继承父进程权限等级。
  - 权限 7 的子进程强制降为 6。
  - 权限 8 的子进程保持 8（仅限系统服务）。
  - 权限 9 的子进程强制降为 6。

4.2 PID 空间分配与系统保留区保护

+----------+----------------------------------------------------------+
| PID 范围 | 用途                                                     |
+----------+----------------------------------------------------------+
| 0        | 内核空闲线程（Idle Thread，每个 CPU 核心一个）            |
| 1～99    | 系统保留区——专供内核及关键系统服务进程使用（如 Init、     |
|          | SecMgr、ServiceHost、Auditd 等）                          |
| 100～999 | 系统服务扩展区（如动态加载的内核辅助线程、监控代理）      |
| 1000及以上| 普通用户态进程（含所有用户应用、兼容程序、沙盒实例）      |
+----------+----------------------------------------------------------+

PID 0-99 硬性访问禁令：
  - 分配层：alloc_pid() 标记 0-99 为 PID_RESERVED，用户态请求返回 -EINVAL。
  - 访问层：任何涉及 PID 0-99 的操作，若调用者不是系统进程，返回 -EPERM
    并记录 CRITICAL 审计。

4.3 系统关键身份（Critical Identity）

task_t 中只读字段 is_critical：
  - PID 1～99 分配时置 1。
  - 权限 9 且通过 OB_CreatePrivilegedProcess 创建时置 1。
  - 任何沙盒进程强制清零。

4.4 系统内部信任锚（ITA）——三层信任模型

  ［第一层：内核固化哈希白名单］
    编译内核时将 /system/critical/ 下核心文件（init.obr、secmgr.obr、
    auditd.obr、kernel_manager.obr）的 SHA-384 硬编码进 .rodata。
    启动早期验证，不匹配则内核 panic。

  ［第二层：系统绑定自签名证书］
    首次启动生成 Ed25519 密钥对。私钥内核只读、永不导出；公钥写入
    OBFS 超级块。官方工具通过 OB_InternalSign 请求内核签名，签名附加到
    .obr 尾部。加载官方程序时验证签名。

  ［第三层：管理员手动哈希白名单］
    权限 9 + UI 确认后，通过 obctl trust add <路径> --hash <SHA384> 加入
    运行时白名单，重启失效。所有操作 CRITICAL 审计。

  优先级：固化哈希 > 系统绑定签名 > 手动白名单。
  沙盒内进程强制跳过第二、三层，仅允许第一层已固化文件。

4.5 双轨沙盒

  ［K-Sandbox：内核态驱动沙盒］
    - 基于 Intel VT-x / AMD-V，独立 EPT/NPT。
    - IOMMU 重映射，DMA 仅限沙盒内存域。
    - 特权指令（MSR/Port I/O）拦截，白名单放行。
    - 崩溃仅杀沙盒实例，不影响主内核。
    - 性能开销 5%~15%，单沙盒内存上限 256MB。
    - 支持 Intel PT 深度踪迹 --trace。

  ［U-Sandbox：用户态应用沙盒］
    - 继承父进程权限，可 --level N 主动降低。
    - 文件系统重定向到 tmpfs，网络返回“网络不可达”。
    - 非白名单系统调用触发 SIGKILL + CRITICAL 审计。
    - 资源限制：CPU 默认 20%，内存默认 512MB，子进程 ≤3。
    - 等级 9 不允许进入沙盒，创建时自动降级至 6。

4.6 兼容层与统一可执行加载器

  - UEL 识别 PE、ELF、.obr 三种格式。
  - ART（API 重定向表）：键为 API 名称 SHA-256 截断，值为函数指针。
    启动晚期由 Init 预加载约 2000 个 Windows API + 300 个 Linux 系统调用。
  - 环境欺骗引擎：Windows PEB（0x7FFE0000）、Linux auxv。
  - 路径转换：
      C:\ → /winmount/C/
      HKLM → /system/registry/HKLM/
      /proc、/sys、/dev → 虚拟内存节点
  - 所有模拟系统调用必须先调用 OB_CheckAccessNative()，
    权限拒绝不得假装成功。

4.7 文件系统 OBFS

  磁盘结构：
    +-------------+------------+----------------+-------------------+
    | 超级块 (4K) | 位图区     | inode表区      | 数据块区          |
    +-------------+------------+----------------+-------------------+
  超级块魔数：0x4F424653 ("OBFS")
  块大小：4K
  inode：256 字节，12 直接块 + 1 一级间址 + 1 二级间址
  超级块保留字段：ita_public_key[32]

  保护目录：
    /kernel/ 与 /system/kernel/：绝对禁区，任何用户态进程不可访问。
    /system/critical/：特许区，仅 PID 1-99、权限 9 + UI 令牌可访问。
    路径检查顺序：/kernel/ → /system/kernel/ → /system/critical/

4.8 沙盒执行引擎（SEE）

  SEE 是内核一级子系统，生命周期从 _kstart() 开始，早于 PID=1。
  包含：
    - 策略缓存器：无锁 RCU 结构，维护活跃沙盒规则集。
    - 系统调用门控器：嵌入 asm_syscall_entry，调用 see_syscall_interceptor()。
    - 逃逸检测器：触发时写内核环形日志并 do_exit(SIGKILL)。
    - 资源回收器：see_cleanup_sandbox() 释放物理页帧、tmpfs inode、
      IOMMU 映射。

4.9 稳定性：Watchdog 与自愈

  - 每 100ms 心跳查询 PID 1,2,3,4。
  - 超过 300ms 未响应，杀死并重启。
  - 重启失败超过 3 次，进入救援模式：仅 OShell，禁止外部应用。

===============================================================================
5. 系统启动流程（x64 UEFI）
===============================================================================

  1. UEFI 固件加载 BootLoader（FAT32 读取）。
  2. BootLoader 进入 32 位保护模式，启用 PAE，切换到 x64 长模式。
  3. 加载内核映像 Knot 到高地址（0xFFFF800000000000 或
     0xFFFFFFFF80000000，以实现为准）。
  4. 跳转到 _kstart()：
     a. 初始化 GDT 和 TSS。
     b. 初始化 IDT，注册异常、IRQ、系统调用门。
     c. 初始化物理内存管理器（伙伴系统）。
     d. 初始化 SLAB 分配器。
     e. 初始化 SEE。
     f. 内核驱动加载三阶段：
          阶段一：引导驱动（NVMe/AHCI/VirtIO）
          阶段二：根文件系统依赖驱动
          阶段三：全硬件枚举，官方驱动主空间，第三方移交 K-Sandbox
     g. 挂载根文件系统（OBFS）。
     h. 创建 /system/critical/ 只读挂载。
     i. 启动 PID=1 Init（权限 8，关键身份）。
     j. 启动 PID=2 SecMgr（权限 8，关键身份）。
     k. 启动 PID=3 ServiceHost（权限 7，关键身份）。
     l. 启动 PID=4 Auditd（权限 8，关键身份）。
     m. Init 进入启动晚期：
          - 解析 /system/init/obinit.toml
          - 按依赖拓扑启动用户态服务
          - 调用 OB_Compat_Preload() 预加载 ART、PEB/auxv 模板
          - 启动内核管理器（/system/critical/kernel_manager.obr，
            权限 9，需 UI 令牌）
     n. 调用 idling 进入调度循环。

===============================================================================
6. 关键系统调用（精确定义）
===============================================================================

常规进程管理：
  OB_CreateProcess(path, args, env, privilege_request) -> pid
  OB_TerminateProcess(pid, exit_code) -> status
  OB_OpenFile(path, flags, mode) -> fd
  OB_ReadFile(fd, buf, count) -> bytes_read
  OB_WriteFile(fd, buf, count) -> bytes_written
  OB_CloseHandle(handle) -> status
  OB_VirtualAlloc(size, flags) -> addr
  OB_VirtualFree(addr) -> status
  OB_GetCurrentToken() -> token_ptr
  OB_CheckAccess(resource_id, mode) -> bool

驱动与沙盒：
  OB_LoadDriver(path) -> status
  OB_LoadDriverSandboxed(path, flags) -> sandbox_id
  OB_RegisterInterrupt(irq, handler) -> status
  OB_CreateSandboxProcess(target_path, args, env) -> sandbox_pid

兼容层预热：
  OB_Compat_Preload() -> status

系统管理（仅等级 9 + UI 令牌）：
  OB_ReadKernelMemory(addr, buf, len) -> bytes_read
  OB_WriteKernelMemory(addr, buf, len) -> bytes_written
  OB_ReadKernelFile(path, buf, len) -> bytes_read
  OB_InternalSign(file_path, output_sig_buf) -> status

所有系统调用在内部均执行权限检查、内核目录保护和 PID 保护。

===============================================================================
7. 开发路线图（10 年阶段）
===============================================================================

  第 1 年：内核基础
  第 2 年：进程与安全底座
  第 3 年：系统服务与工具链
  第 4 年：隔离与沙盒
  第 5 年：兼容层完整
  第 6 年：驱动框架与 IOMMU
  第 7 年：K-Sandbox
  第 8 年：物理机与多核
  第 9 年：生态与完整兼容
  第 10 年：安全、稳定与发布

关键里程碑：
  M1  内核启动：第 6 月
  M2  权限引擎：第 21 月
  M3  Init 启动：第 30 月
  M4  .obr 运行：第 33 月
  M5  U-Sandbox：第 42 月
  M6  Linux ELF：第 51 月
  M7  Win32 子集：第 54 月
  M8  K-Sandbox：第 84 月
  M9  物理机：第 90 月
  M10 v1.0：第 120 月

合理周期：120 个月（10 年）
保守周期：144 个月（12 年）
若压缩到 60 个月，只能实现裁剪版 v1.0。

===============================================================================
8. 当前开发进度（截至当前仓库）
===============================================================================

已完成：
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

进行中：
  - 第 4 步：GDT/TSS/IDT/异常/系统调用门（约 30%~40%）
    已具备：GDT、IDT 异常桩、串口
    缺失：TSS、IRQ、PIC/APIC、定时器、系统调用门、可恢复异常处理

超前实现：
  - 第 5/6 步部分内容：PMM、VMM、kmalloc 已可运行

当前串口日志示例：
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
9. 构建与运行环境
===============================================================================

9.1 必需工具

  - MSYS2（UCRT64 或 MINGW64）
  - clang / LLVM
  - lld-link
  - nasm
  - binutils
  - make / cmake / ninja
  - QEMU for Windows
  - OVMF 固件（需手动下载，见 9.3 节）

9.2 安装工具链

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

9.3 OVMF 获取（必须手动下载）

注意：
  MSYS2 仓库中通常没有 edk2-ovmf 包，OVMF 固件必须手动下载。
  请在 MSYS2 UCRT64 Shell 中执行，不要使用 PowerShell 或 CMD。

步骤：
  1) 打开 MSYS2 UCRT64 Shell。
  2) 进入项目根目录，例如：
       cd /d/OmniBridgeOs
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
  - 新开 Shell 后需重新执行 export，或写入 ~/.bashrc。
  - 如果使用 PowerShell 的 run-qemu.ps1，需设置 Windows 路径，例如：
      $env:OVMF_PATH = "D:\OmniBridgeOs\OVMF.fd"
  - run-qemu.sh 会自动把 MSYS 路径转换为 Windows 路径。
  - 如果 curl 不存在，可先安装：
      pacman -S curl

9.4 构建

在 PowerShell 中：
  .\scripts\build.ps1

该脚本会调用 MSYS2 中的 make all，最终生成：
  build/esp/EFI/BOOT/BOOTX64.EFI

9.5 运行 QEMU

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

9.6 校验串口日志

  .\scripts\check-serial.ps1

当前脚本要求日志中包含：
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

注意：实际日志中输出为 Hello Kernel，而脚本要求 [KRN] Hello Kernel，
存在前缀不一致，后续需统一。

===============================================================================
10. 目录结构
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
│   │   ├── boot.h            # 内核 LMA/VMA 与 boot_info
│   │   ├── pmm.c / pmm.h     # 伙伴系统物理内存管理
│   │   ├── vmm.c / vmm.h     # 4 级页表与内核高半区映射
│   │   ├── kmalloc.c / kmalloc.h # SLAB 分配器
│   │   └── main.c                # _kstart_c 内核 C 入口
├── scripts/
│   ├── build.ps1
│   ├── run-qemu.ps1
│   ├── run-qemu.sh
│   ├── setup-toolchain.ps1
│   ├── setup-toolchain.sh
│   ├── check-serial.ps1
│   └── pe_to_elf.py            # PE32+ -> ELF64 转换工具
├── tests/
│   ├── test_serial.c
│   ├── test_vmm.c
│   ├── test_pmm.c
│   └── test_kmalloc.c
├── build/                    # 构建输出
│   ├── esp/EFI/BOOT/BOOTX64.EFI
│   └── serial.log
└── README.txt

===============================================================================
11. 测试
===============================================================================

宿主侧测试可直接编译运行，例如：

  gcc -I kernel/arch/x64 tests/test_serial.c kernel/arch/x64/kprintf.c -o test_serial
  ./test_serial

其他测试类似。

QEMU 内测试通过串口日志校验脚本进行：

  .\scripts\check-serial.ps1

===============================================================================
文档结束
===============================================================================
