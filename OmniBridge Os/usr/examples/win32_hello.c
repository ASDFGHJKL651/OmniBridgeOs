/*===OmniBridgeOs/usr/examples/win32_hello.c===*/
/*
 * win32_hello —— 简单 Win32 console hello world。
 *
 * 编译（由 Makefile 自动执行；此处仅作文档）：
 *
 *   x86_64-w64-mingw32-gcc -nostdlib -ffreestanding -O2 \
 *       -Wl,-e,mainCRTStartup_win32 -Wl,--subsystem,console \
 *       -Wl,--image-base,0x140000000 \
 *       -o win32_hello.exe win32_hello.c -lkernel32
 *
 * 或：
 *
 *   clang --target=x86_64-pc-windows-gnu -nostdlib -ffreestanding -O2 \
 *       -Wl,-e,mainCRTStartup_win32 -Wl,--subsystem,console \
 *       -Wl,--image-base,0x140000000 \
 *       -o win32_hello.exe win32_hello.c -lkernel32
 *
 * 本程序不依赖 CRT（-nostdlib）。在真实 Windows 下缺 CRT 无法运行，
 * 但在 OmniBridge 下由 PE 加载器直接调用 entry，且 CRT 初始化被省略。
 *
 * 人工必须审查：
 *   - 函数原型 __stdcall 在本步无实际意义（x64 只有一种调用约定）；
 *     但符号名约束仍生效，Makefile 中需显式 -Wl,-e,mainCRTStartup_win32。
 */

typedef unsigned int   DWORD;
typedef int            BOOL;
typedef void          *HANDLE;
typedef const char    *LPCSTR;

/* Windows 常量 */
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define TRUE              1

/* 从 kernel32.dll 导入 */
__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD nStdHandle);
__declspec(dllimport) BOOL   __stdcall WriteFile(HANDLE hFile,
                                                 const void *lpBuffer,
                                                 DWORD nNumberOfBytesToWrite,
                                                 DWORD *lpNumberOfBytesWritten,
                                                 void *lpOverlapped);
__declspec(dllimport) void   __stdcall ExitProcess(DWORD uExitCode);

void __stdcall mainCRTStartup_win32(void);

void __stdcall mainCRTStartup_win32(void)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    const char *msg = "Hello from Win32!\r\n";
    DWORD written = 0;
    WriteFile(hOut, msg, 19, &written, 0);
    ExitProcess(0);
}