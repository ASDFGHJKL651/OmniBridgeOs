/*===OmniBridgeOs/usr/examples/win32_api_test.c===*/
/*
 * win32_api_test —— Win32 API 子集综合验收测试（第 20 步扩展）。
 *
 * 覆盖 A 类真实实现（全部限定在 kernel32 命名空间）：
 *   句柄    : GetStdHandle / CloseHandle
 *   错误    : GetLastError / SetLastError
 *   文件    : CreateDirectoryA / CreateFileA / WriteFile / ReadFile /
 *             GetFileSize / SetFilePointer
 *   进程    : GetCurrentProcessId / GetCurrentThreadId / GetTickCount
 *   堆      : GetProcessHeap / HeapAlloc / HeapFree
 *   虚存    : VirtualAlloc / VirtualFree
 *   模块    : GetModuleHandleA
 *   系统    : GetVersion / GetComputerNameA
 *   kernel32: lstrlenA / lstrcpyA / lstrcatA / lstrcmpA
 *   退出    : ExitProcess
 *
 * ★ 设计约束（人工必须审查）：
 *   1) 本程序**不**依赖 msvcrt.dll / advapi32.dll —— 只导入 kernel32。
 *      memset / memcpy / memcmp / strlen / strcmp 全部自实现（见下方
 *      my_* 辅助函数）。原因：
 *        - MSVC/Clang 对 __declspec(dllimport) 的 msvcrt 符号会生成
 *          __imp_memset 等重定位项，需要 -lmsvcrt 才能链接；
 *        - 为减少构建依赖，我们直接把小函数内联化。
 *   2) 自实现的函数加 __attribute__((noinline)) + volatile 指针，
 *      避免编译器把它们优化回内建的 memset/memcpy 调用。
 *   3) 入口符号 mainCRTStartup_win32；ImageBase = 0x8000000000；
 *      与 win32_hello.exe 一致。
 */

typedef unsigned int        DWORD;
typedef int                 BOOL;
typedef void               *HANDLE;
typedef void               *LPVOID;
typedef void               *LPSECURITY_ATTRIBUTES;
typedef const char         *LPCSTR;
typedef char               *LPSTR;
typedef unsigned long long  ULONGLONG;
typedef unsigned long long  SIZE_T;

#define WINAPI __stdcall
#define TRUE   1
#define FALSE  0

#define INVALID_HANDLE_VALUE  ((HANDLE)(long long)-1)

/* 标准句柄 */
#define STD_INPUT_HANDLE   ((DWORD)-10)
#define STD_OUTPUT_HANDLE  ((DWORD)-11)
#define STD_ERROR_HANDLE   ((DWORD)-12)

/* CreateFileA flags */
#define GENERIC_READ      0x80000000UL
#define GENERIC_WRITE     0x40000000UL

/* CreateFileA disposition */
#define CREATE_NEW        1
#define CREATE_ALWAYS     2
#define OPEN_EXISTING     3

#define FILE_ATTRIBUTE_NORMAL 0x80

/* SetFilePointer method */
#define FILE_BEGIN        0
#define FILE_CURRENT      1
#define FILE_END          2

/* ============================================================
 * 自实现的小函数（不依赖 msvcrt）
 *
 * 人工必须审查：
 *   - __attribute__((noinline)) 阻止编译器把循环识别为内建 memset。
 *   - volatile 指针防止死代码消除。
 *   - 语义与 C 标准库一致（但本程序内不对外暴露）。
 * ============================================================ */

__attribute__((noinline))
static void *my_memset(void *dest, int c, SIZE_T n)
{
    volatile unsigned char *d = (volatile unsigned char *)dest;
    unsigned char v = (unsigned char)c;
    for (SIZE_T i = 0; i < n; ++i) d[i] = v;
    return dest;
}

__attribute__((noinline))
static void *my_memcpy(void *dest, const void *src, SIZE_T n)
{
    volatile unsigned char *d = (volatile unsigned char *)dest;
    const volatile unsigned char *s = (const volatile unsigned char *)src;
    for (SIZE_T i = 0; i < n; ++i) d[i] = s[i];
    return dest;
}

__attribute__((noinline))
static int my_memcmp(const void *a, const void *b, SIZE_T n)
{
    const volatile unsigned char *x = (const volatile unsigned char *)a;
    const volatile unsigned char *y = (const volatile unsigned char *)b;
    for (SIZE_T i = 0; i < n; ++i) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

__attribute__((noinline))
static SIZE_T my_strlen(const char *s)
{
    SIZE_T n = 0;
    if (!s) return 0;
    volatile const char *p = s;
    while (p[n]) ++n;
    return n;
}

__attribute__((noinline))
static int my_strcmp(const char *a, const char *b)
{
    volatile const char *x = a;
    volatile const char *y = b;
    while (*x && *x == *y) { ++x; ++y; }
    return (int)(unsigned char)*x - (int)(unsigned char)*y;
}

/* ============================================================
 * kernel32 导入（唯一使用的 DLL）
 * ============================================================ */
__declspec(dllimport) HANDLE WINAPI GetStdHandle(DWORD nStdHandle);
__declspec(dllimport) BOOL   WINAPI WriteFile(HANDLE hFile,
                                              const void *lpBuffer,
                                              DWORD nNumberOfBytesToWrite,
                                              DWORD *lpNumberOfBytesWritten,
                                              void *lpOverlapped);
__declspec(dllimport) BOOL   WINAPI ReadFile(HANDLE hFile,
                                             void *lpBuffer,
                                             DWORD nNumberOfBytesToRead,
                                             DWORD *lpNumberOfBytesRead,
                                             void *lpOverlapped);
__declspec(dllimport) HANDLE WINAPI CreateFileA(LPCSTR lpFileName,
                                                DWORD dwDesiredAccess,
                                                DWORD dwShareMode,
                                                LPSECURITY_ATTRIBUTES lpSA,
                                                DWORD dwCreationDisposition,
                                                DWORD dwFlagsAndAttributes,
                                                HANDLE hTemplateFile);
__declspec(dllimport) BOOL   WINAPI CreateDirectoryA(LPCSTR lpPathName,
                                                     LPSECURITY_ATTRIBUTES lpSA);
__declspec(dllimport) BOOL   WINAPI CloseHandle(HANDLE hObject);
__declspec(dllimport) DWORD  WINAPI GetFileSize(HANDLE hFile,
                                                DWORD *lpFileSizeHigh);
__declspec(dllimport) DWORD  WINAPI SetFilePointer(HANDLE hFile,
                                                   long lDistanceToMove,
                                                   long *lpDistanceToMoveHigh,
                                                   DWORD dwMoveMethod);
__declspec(dllimport) DWORD  WINAPI GetLastError(void);
__declspec(dllimport) void   WINAPI SetLastError(DWORD dwErrCode);
__declspec(dllimport) DWORD  WINAPI GetCurrentProcessId(void);
__declspec(dllimport) DWORD  WINAPI GetCurrentThreadId(void);
__declspec(dllimport) DWORD  WINAPI GetTickCount(void);
__declspec(dllimport) HANDLE WINAPI GetProcessHeap(void);
__declspec(dllimport) LPVOID WINAPI HeapAlloc(HANDLE hHeap, DWORD dwFlags,
                                              SIZE_T dwBytes);
__declspec(dllimport) BOOL   WINAPI HeapFree(HANDLE hHeap, DWORD dwFlags,
                                             LPVOID lpMem);
__declspec(dllimport) LPVOID WINAPI VirtualAlloc(LPVOID lpAddress,
                                                 SIZE_T dwSize,
                                                 DWORD flAllocationType,
                                                 DWORD flProtect);
__declspec(dllimport) BOOL   WINAPI VirtualFree(LPVOID lpAddress,
                                                SIZE_T dwSize,
                                                DWORD dwFreeType);
__declspec(dllimport) HANDLE WINAPI GetModuleHandleA(LPCSTR lpModuleName);
__declspec(dllimport) DWORD  WINAPI GetVersion(void);
__declspec(dllimport) BOOL   WINAPI GetComputerNameA(LPSTR lpBuffer,
                                                     DWORD *nSize);
__declspec(dllimport) int    WINAPI lstrlenA(LPCSTR lpString);
__declspec(dllimport) LPSTR  WINAPI lstrcpyA(LPSTR lpString1,
                                             LPCSTR lpString2);
__declspec(dllimport) LPSTR  WINAPI lstrcatA(LPSTR lpString1,
                                             LPCSTR lpString2);
__declspec(dllimport) int    WINAPI lstrcmpA(LPCSTR lpString1,
                                             LPCSTR lpString2);
__declspec(dllimport) void   WINAPI ExitProcess(DWORD uExitCode);

/* ============================================================
 * 辅助：输出
 * ============================================================ */

static int g_fail = 0;
static int g_pass = 0;

static void out_str(LPCSTR s)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    int n = lstrlenA(s);
    if (n > 0) {
        WriteFile(h, s, (DWORD)n, &written, 0);
    }
}

static void out_dec(DWORD v)
{
    char tmp[16];
    int i = 0;
    if (v == 0) { tmp[i++] = '0'; }
    while (v > 0 && i < 15) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    char out[16];
    int j = 0;
    while (i > 0) out[j++] = tmp[--i];
    out[j] = '\0';
    out_str(out);
}

/* ★ 修复：b[10] 装不下 '0','x',8 位 hex,'\0' 共 11 字节。 */
static void out_hex8(DWORD v)
{
    const char *hx = "0123456789abcdef";
    char b[11];
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 8; ++i) {
        b[2 + i] = hx[(v >> ((7 - i) * 4)) & 0xF];
    }
    b[10] = '\0';
    out_str(b);
}

static void check(const char *desc, int ok)
{
    if (ok) {
        g_pass++;
        out_str("[OK]   ");
    } else {
        g_fail++;
        out_str("[FAIL] ");
    }
    out_str(desc);
    out_str("\n");
}

/* ============================================================
 * 测试段
 * ============================================================ */

static void test_std_handle(void)
{
    out_str("[win32_api_test] test_std_handle\n");

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    check("GetStdHandle(STD_OUTPUT_HANDLE) == 1", (long long)hOut == 1);

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    check("GetStdHandle(STD_INPUT_HANDLE) == 0", (long long)hIn == 0);

    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    check("GetStdHandle(STD_ERROR_HANDLE) == 2", (long long)hErr == 2);

    HANDLE hBad = GetStdHandle(999);
    check("GetStdHandle(999) == INVALID_HANDLE_VALUE",
          hBad == INVALID_HANDLE_VALUE);
}

static void test_error_code(void)
{
    out_str("[win32_api_test] test_error_code\n");

    SetLastError(0);
    check("GetLastError() == 0 after SetLastError(0)",
          GetLastError() == 0);

    SetLastError(1234);
    check("SetLastError(1234) roundtrip", GetLastError() == 1234);

    SetLastError(0xDEADBEEF);
    check("SetLastError(0xDEADBEEF) roundtrip",
          GetLastError() == 0xDEADBEEF);
}

static void test_process_info(void)
{
    out_str("[win32_api_test] test_process_info\n");

    DWORD pid  = GetCurrentProcessId();
    DWORD tid  = GetCurrentThreadId();
    DWORD tick = GetTickCount();

    out_str("  pid  = "); out_dec(pid);  out_str("\n");
    out_str("  tid  = "); out_dec(tid);  out_str("\n");
    out_str("  tick = "); out_dec(tick); out_str("\n");

    check("GetCurrentProcessId() != 0", pid != 0);
    check("GetCurrentThreadId() != 0",  tid != 0);
    check("GetTickCount() callable", 1);
}

static void test_module(void)
{
    out_str("[win32_api_test] test_module\n");

    HANDLE hMod = GetModuleHandleA(0);
    out_str("  ImageBase = "); out_hex8((DWORD)(ULONGLONG)hMod); out_str("\n");
    check("GetModuleHandleA(NULL) != 0", (long long)hMod != 0);
}

static void test_heap(void)
{
    out_str("[win32_api_test] test_heap\n");

    HANDLE hHeap = GetProcessHeap();
    check("GetProcessHeap() != 0", (long long)hHeap != 0);

    LPVOID p = HeapAlloc(hHeap, 0, 512);
    check("HeapAlloc(512) != 0", p != 0);

    if (p) {
        unsigned char *b = (unsigned char *)p;
        my_memset(p, 0xAB, 512);
        check("HeapAlloc buffer writable",
              b[0] == 0xAB && b[255] == 0xAB && b[511] == 0xAB);

        BOOL freed = HeapFree(hHeap, 0, p);
        check("HeapFree returns TRUE", freed == TRUE);
    }
}

static void test_virtual_alloc(void)
{
    out_str("[win32_api_test] test_virtual_alloc\n");

    /* MEM_COMMIT | MEM_RESERVE = 0x3000, PAGE_READWRITE = 0x04 */
    LPVOID p = VirtualAlloc(0, 4096, 0x3000, 0x04);
    check("VirtualAlloc(4096) != 0", p != 0);

    if (p) {
        unsigned char *b = (unsigned char *)p;
        b[0]    = 0xCD;
        b[4095] = 0xEF;
        check("VirtualAlloc memory writable",
              b[0] == 0xCD && b[4095] == 0xEF);
        /* MEM_RELEASE = 0x8000 */
        BOOL freed = VirtualFree(p, 0, 0x8000);
        check("VirtualFree returns TRUE", freed == TRUE);
    }
}

static void test_file_io(void)
{
    out_str("[win32_api_test] test_file_io\n");

    /*
     * 使用 %TEMP% 作为基目录：
     *   %TEMP%           -> /tmp/win_temp
     *   %TEMP%\foo.txt   -> /tmp/win_temp/foo.txt
     * 父目录 /tmp 在 main.c 的 embed_install_programs 中已创建；
     * /tmp/win_temp 由本程序 CreateDirectoryA 创建。
     */
    CreateDirectoryA("%TEMP%", 0);

    const char *path = "%TEMP%\\win32_api_test.dat";
    const char *payload = "OmniBridge-Win32-API-Test-0123456789";
    int plen = lstrlenA(payload);

    /* ---- 1) 创建（CREATE_ALWAYS） ---- */
    HANDLE h = CreateFileA(path,
                           GENERIC_READ | GENERIC_WRITE,
                           0, 0, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, 0);
    check("CreateFileA(CREATE_ALWAYS) succeeds",
          h != INVALID_HANDLE_VALUE);
    if (h == INVALID_HANDLE_VALUE) {
        out_str("  GetLastError = "); out_dec(GetLastError()); out_str("\n");
        return;
    }

    /* ---- 2) 写 ---- */
    DWORD written = 0;
    BOOL ok = WriteFile(h, payload, (DWORD)plen, &written, 0);
    check("WriteFile returns TRUE", ok == TRUE);
    check("WriteFile wrote all bytes", written == (DWORD)plen);

    /* ---- 3) 关闭 ---- */
    BOOL closed = CloseHandle(h);
    check("CloseHandle succeeds", closed == TRUE);

    /* ---- 4) 重开读（OPEN_EXISTING） ---- */
    h = CreateFileA(path, GENERIC_READ, 0, 0, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, 0);
    check("CreateFileA(OPEN_EXISTING) succeeds",
          h != INVALID_HANDLE_VALUE);
    if (h == INVALID_HANDLE_VALUE) return;

    /* ---- 5) GetFileSize ---- */
    DWORD sz = GetFileSize(h, 0);
    out_str("  file size = "); out_dec(sz); out_str("\n");
    check("GetFileSize == payload length", sz == (DWORD)plen);

    /* ---- 6) ReadFile ---- */
    char buf[128];
    my_memset(buf, 0, sizeof(buf));
    DWORD nr = 0;
    ok = ReadFile(h, buf, (DWORD)plen, &nr, 0);
    check("ReadFile returns TRUE", ok == TRUE);
    check("ReadFile read all bytes", nr == (DWORD)plen);
    buf[plen] = '\0';
    check("ReadFile content matches payload",
          my_strcmp(buf, payload) == 0);

    /* ---- 7) SetFilePointer(FILE_BEGIN, 0) ---- */
    DWORD np = SetFilePointer(h, 0, 0, FILE_BEGIN);
    check("SetFilePointer(FILE_BEGIN, 0) returns 0", np == 0);

    CloseHandle(h);
}

static void test_string(void)
{
    out_str("[win32_api_test] test_string\n");

    /* kernel32 lstr* */
    check("lstrlenA(\"hello\") == 5", lstrlenA("hello") == 5);
    check("lstrlenA(\"\") == 0",      lstrlenA("") == 0);

    char buf[32];
    lstrcpyA(buf, "abc");
    check("lstrcpyA(\"abc\")", my_strcmp(buf, "abc") == 0);

    lstrcatA(buf, "def");
    check("lstrcatA(\"def\")", my_strcmp(buf, "abcdef") == 0);

    check("lstrcmpA(equal) == 0",     lstrcmpA("xyz", "xyz") == 0);
    check("lstrcmpA(different) != 0", lstrcmpA("xyz", "abc") != 0);

    /* 自实现字符串函数（验证 my_* 与 lstr* 行为一致） */
    check("my_strlen(\"abcdef\") == 6", my_strlen("abcdef") == 6);
    check("my_strcmp(equal) == 0",      my_strcmp("hello", "hello") == 0);
    check("my_strcmp(different) != 0",  my_strcmp("hello", "world") != 0);

    char a[8], b[8];
    my_memset(a, 0x41, 8);
    my_memcpy(b, a, 8);
    check("my_memcmp(my_memcpy) == 0", my_memcmp(a, b, 8) == 0);
    b[7] = 0x42;
    check("my_memcmp(differ) != 0", my_memcmp(a, b, 8) != 0);
}

static void test_system_info(void)
{
    out_str("[win32_api_test] test_system_info\n");

    DWORD v = GetVersion();
    out_str("  GetVersion = "); out_hex8(v); out_str("\n");
    check("GetVersion() != 0", v != 0);

    /* 人工必须审查：GetUserNameA 属于 advapi32.dll，本程序为避免
     * 依赖额外的 .a 库，不再调用它。若需要测试，请在 Makefile 链接
     * 命令中加入 -ladvapi32。 */
    char name[64];
    DWORD n = sizeof(name);
    if (GetComputerNameA(name, &n)) {
        name[n < 64 ? n : 63] = '\0';
        out_str("  computer = "); out_str(name); out_str("\n");
        check("GetComputerNameA non-empty", n > 0);
    } else {
        check("GetComputerNameA non-empty", 0);
    }
}

/* ============================================================
 * 入口
 * ============================================================ */

void mainCRTStartup_win32(void);

void mainCRTStartup_win32(void)
{
    out_str("=== win32_api_test ===\n");

    test_std_handle();
    test_error_code();
    test_process_info();
    test_module();
    test_heap();
    test_virtual_alloc();
    test_file_io();
    test_string();
    test_system_info();

    out_str("=== pass=");
    out_dec((DWORD)g_pass);
    out_str(" fail=");
    out_dec((DWORD)g_fail);
    out_str(" ===\n");

    if (g_fail == 0) {
        out_str("=== ALL PASS ===\n");
        ExitProcess(0);
    } else {
        out_str("=== FAILURES ===\n");
        ExitProcess(1);
    }
}
/*===OmniBridgeOs/usr/examples/win32_api_test.c 结束===*/