/*===OmniBridgeOs/kernel/arch/x64/user/win32_api.h===*/
/*
 * Win32 API 桩分发 —— 第 20 步扩展版（500 个 API）。
 *
 * 人工必须审查（关键不变量）：
 *   1) API 编号从 0x2001 起，共 500 项，最后一项为 0x21F4。
 *   2) 编号区间必须与 Linux syscall (0..~450) 和 OB syscall
 *      (0x100..0x15E) 完全无重叠。
 *   3) 所有 API 入口都必须经 check_permission；
 *      拒绝时返回 Windows 错误码并写审计。
 *   4) 未实现的 API 返回 ERROR_CALL_NOT_IMPLEMENTED，
 *      绝不以 TRUE / 0 假装成功。
 *   5) 名称规范化逻辑（大小写不敏感、去 .dll、去前导下划线、
 *      去 @N 装饰）与 win32_api_lookup 一致。
 */
#ifndef OMNIBRIDGE_USER_WIN32_API_H
#define OMNIBRIDGE_USER_WIN32_API_H

#include <stdint.h>
#include "task.h"
#include "syscall.h"

/* ---------- API 编号范围 ---------- */
#define WIN32_API_FIRST  0x2001u
#define WIN32_API_LAST   0x21F4u
#define WIN32_API_COUNT  500u

/*
 * 编译期断言（人工必须审查）：
 *   若修改 WIN32_API_FIRST/LAST/COUNT，此断言会强制三者一致。
 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(WIN32_API_LAST - WIN32_API_FIRST + 1u == WIN32_API_COUNT,
               "WIN32_API_COUNT must equal (LAST - FIRST + 1)");
#endif

/* ============================================================
 * 原有 32 项（0x2001 .. 0x2020，保持不变）
 * ============================================================ */
#define WIN32_API_GetStdHandle           0x2001u
#define WIN32_API_WriteFile              0x2002u
#define WIN32_API_ReadFile               0x2003u
#define WIN32_API_ExitProcess            0x2004u
#define WIN32_API_GetLastError           0x2005u
#define WIN32_API_SetLastError           0x2006u
#define WIN32_API_CreateFileA            0x2007u
#define WIN32_API_CreateFileW            0x2008u
#define WIN32_API_CloseHandle            0x2009u
#define WIN32_API_GetFileSize            0x200Au
#define WIN32_API_GetFileSizeEx          0x200Bu
#define WIN32_API_SetFilePointer         0x200Cu
#define WIN32_API_SetFilePointerEx       0x200Du
#define WIN32_API_GetCurrentProcessId    0x200Eu
#define WIN32_API_GetCurrentThreadId     0x200Fu
#define WIN32_API_GetTickCount           0x2010u
#define WIN32_API_Sleep                  0x2011u
#define WIN32_API_GetCommandLineA        0x2012u
#define WIN32_API_GetCommandLineW        0x2013u
#define WIN32_API_GetEnvironmentStrings  0x2014u
#define WIN32_API_GetModuleHandleA       0x2015u
#define WIN32_API_GetModuleHandleW       0x2016u
#define WIN32_API_GetModuleFileNameA     0x2017u
#define WIN32_API_GetModuleFileNameW     0x2018u
#define WIN32_API_HeapAlloc              0x2019u
#define WIN32_API_HeapFree               0x201Au
#define WIN32_API_GetProcessHeap         0x201Bu
#define WIN32_API_GetStartupInfoA        0x201Cu
#define WIN32_API_GetStartupInfoW        0x201Du
#define WIN32_API_VirtualAlloc           0x201Eu
#define WIN32_API_VirtualFree            0x201Fu
#define WIN32_API_GetVersion             0x2020u

/* ============================================================
 * kernel32（附加）
 * ============================================================ */
#define WIN32_API_SetStdHandle            0x2021u
#define WIN32_API_GetTickCount64          0x2022u
#define WIN32_API_SleepEx                 0x2023u
#define WIN32_API_TerminateProcess        0x2024u
#define WIN32_API_GetExitCodeProcess      0x2025u
#define WIN32_API_GetExitCodeThread       0x2026u
#define WIN32_API_FlushFileBuffers        0x2027u
#define WIN32_API_DeleteFileA             0x2028u
#define WIN32_API_DeleteFileW             0x2029u
#define WIN32_API_MoveFileA               0x202Au
#define WIN32_API_MoveFileW               0x202Bu
#define WIN32_API_CopyFileA               0x202Cu
#define WIN32_API_CopyFileW               0x202Du
#define WIN32_API_GetFileAttributesA      0x202Eu
#define WIN32_API_GetFileAttributesW      0x202Fu
#define WIN32_API_SetFileAttributesA      0x2030u
#define WIN32_API_SetFileAttributesW      0x2031u
#define WIN32_API_FindFirstFileA          0x2032u
#define WIN32_API_FindNextFileA           0x2033u
#define WIN32_API_FindClose               0x2034u
#define WIN32_API_GetFullPathNameA        0x2035u
#define WIN32_API_GetFullPathNameW        0x2036u
#define WIN32_API_CreateDirectoryA        0x2037u
#define WIN32_API_CreateDirectoryW        0x2038u
#define WIN32_API_RemoveDirectoryA        0x2039u
#define WIN32_API_RemoveDirectoryW        0x203Au
#define WIN32_API_GetCurrentDirectoryA    0x203Bu
#define WIN32_API_GetCurrentDirectoryW    0x203Cu
#define WIN32_API_SetCurrentDirectoryA    0x203Du
#define WIN32_API_SetCurrentDirectoryW    0x203Eu
#define WIN32_API_GetFileType             0x203Fu
#define WIN32_API_SetEndOfFile            0x2040u
#define WIN32_API_HeapCreate              0x2041u
#define WIN32_API_HeapDestroy             0x2042u
#define WIN32_API_HeapReAlloc             0x2043u
#define WIN32_API_HeapSize                0x2044u
#define WIN32_API_VirtualAllocEx          0x2045u
#define WIN32_API_VirtualFreeEx           0x2046u
#define WIN32_API_VirtualProtect          0x2047u
#define WIN32_API_VirtualQuery            0x2048u
#define WIN32_API_GlobalAlloc             0x2049u
#define WIN32_API_GlobalFree              0x204Au
#define WIN32_API_LocalAlloc              0x204Bu
#define WIN32_API_LocalFree               0x204Cu
#define WIN32_API_CreateProcessA          0x204Du
#define WIN32_API_CreateProcessW          0x204Eu
#define WIN32_API_OpenProcess             0x204Fu
#define WIN32_API_GetCurrentProcess       0x2050u
#define WIN32_API_GetCurrentThread        0x2051u
#define WIN32_API_CreateThread            0x2052u
#define WIN32_API_ExitThread              0x2053u
#define WIN32_API_WaitForSingleObject     0x2054u
#define WIN32_API_WaitForMultipleObjects  0x2055u
#define WIN32_API_SetEvent                0x2056u
#define WIN32_API_ResetEvent              0x2057u
#define WIN32_API_CreateEventA            0x2058u
#define WIN32_API_CreateEventW            0x2059u
#define WIN32_API_CreateMutexA            0x205Au
#define WIN32_API_CreateMutexW            0x205Bu
#define WIN32_API_ReleaseMutex            0x205Cu
#define WIN32_API_InitializeCriticalSection 0x205Du
#define WIN32_API_EnterCriticalSection    0x205Eu
#define WIN32_API_LeaveCriticalSection    0x205Fu
#define WIN32_API_DeleteCriticalSection   0x2060u
#define WIN32_API_SuspendThread           0x2061u
#define WIN32_API_ResumeThread            0x2062u
#define WIN32_API_GetThreadPriority       0x2063u
#define WIN32_API_SetThreadPriority       0x2064u
#define WIN32_API_GetVersionExA           0x2065u
#define WIN32_API_GetVersionExW           0x2066u
#define WIN32_API_GetEnvironmentVariableA 0x2067u
#define WIN32_API_GetEnvironmentVariableW 0x2068u
#define WIN32_API_SetEnvironmentVariableA 0x2069u
#define WIN32_API_SetEnvironmentVariableW 0x206Au
#define WIN32_API_GetTempPathA            0x206Bu
#define WIN32_API_GetTempPathW            0x206Cu
#define WIN32_API_GetTempFileNameA        0x206Du
#define WIN32_API_GetTempFileNameW        0x206Eu
#define WIN32_API_GetSystemDirectoryA     0x206Fu
#define WIN32_API_GetSystemDirectoryW     0x2070u
#define WIN32_API_GetWindowsDirectoryA    0x2071u
#define WIN32_API_GetWindowsDirectoryW    0x2072u
#define WIN32_API_GetSystemInfo           0x2073u
#define WIN32_API_GetSystemTime           0x2074u
#define WIN32_API_GetLocalTime            0x2075u
#define WIN32_API_SystemTimeToFileTime    0x2076u
#define WIN32_API_FileTimeToSystemTime    0x2077u
#define WIN32_API_QueryPerformanceCounter   0x2078u
#define WIN32_API_QueryPerformanceFrequency 0x2079u
#define WIN32_API_GetComputerNameA        0x207Au
#define WIN32_API_GetComputerNameW        0x207Bu
#define WIN32_API_GetUserNameA            0x207Cu
#define WIN32_API_GetUserNameW            0x207Du
#define WIN32_API_GetSystemTimeAsFileTime 0x207Eu
#define WIN32_API_GetSystemTimes          0x207Fu
#define WIN32_API_GetProcessTimes         0x2080u
#define WIN32_API_GetThreadTimes          0x2081u
#define WIN32_API_GetACP                  0x2082u
#define WIN32_API_GetOEMCP                0x2083u
#define WIN32_API_GetCPInfo               0x2084u
#define WIN32_API_GetConsoleCP            0x2085u
#define WIN32_API_GetConsoleOutputCP      0x2086u
#define WIN32_API_GetConsoleMode          0x2087u
#define WIN32_API_SetConsoleMode          0x2088u
#define WIN32_API_GetConsoleScreenBufferInfo 0x2089u
#define WIN32_API_SetConsoleCursorPosition 0x208Au
#define WIN32_API_SetConsoleTextAttribute 0x208Bu
#define WIN32_API_WriteConsoleA           0x208Cu
#define WIN32_API_WriteConsoleW           0x208Du
#define WIN32_API_ReadConsoleA            0x208Eu
#define WIN32_API_ReadConsoleW            0x208Fu
#define WIN32_API_AllocConsole            0x2090u
#define WIN32_API_FreeConsole             0x2091u
#define WIN32_API_GetConsoleWindow        0x2092u
#define WIN32_API_lstrlenA                0x2093u
#define WIN32_API_lstrlenW                0x2094u
#define WIN32_API_lstrcpyA                0x2095u
#define WIN32_API_lstrcpyW                0x2096u
#define WIN32_API_lstrcatA                0x2097u
#define WIN32_API_lstrcatW                0x2098u
#define WIN32_API_lstrcmpA                0x2099u
#define WIN32_API_lstrcmpW                0x209Au
#define WIN32_API_lstrcmpiA               0x209Bu
#define WIN32_API_lstrcmpiW               0x209Cu
#define WIN32_API_MulDiv                  0x209Du
#define WIN32_API_IsDebuggerPresent       0x209Eu
#define WIN32_API_OutputDebugStringA      0x209Fu
#define WIN32_API_OutputDebugStringW      0x20A0u
#define WIN32_API_DebugBreak              0x20A1u
#define WIN32_API_GetLogicalDrives        0x20A2u
#define WIN32_API_GetLogicalDriveStringsA 0x20A3u
#define WIN32_API_GetLogicalDriveStringsW 0x20A4u
#define WIN32_API_GetDriveTypeA           0x20A5u
#define WIN32_API_GetDriveTypeW           0x20A6u
#define WIN32_API_GetVolumeInformationA   0x20A7u
#define WIN32_API_GetVolumeInformationW   0x20A8u
#define WIN32_API_GetDiskFreeSpaceA       0x20A9u
#define WIN32_API_GetDiskFreeSpaceW       0x20AAu
#define WIN32_API_GetFileTime             0x20ABu
#define WIN32_API_SetFileTime             0x20ACu
#define WIN32_API_GetFileInformationByHandle 0x20ADu
#define WIN32_API_LockFile                0x20AEu
#define WIN32_API_UnlockFile              0x20AFu
#define WIN32_API_CreatePipe              0x20B0u
#define WIN32_API_PeekNamedPipe           0x20B1u
#define WIN32_API_ReadFileEx              0x20B2u
#define WIN32_API_WriteFileEx             0x20B3u
#define WIN32_API_GetOverlappedResult     0x20B4u
#define WIN32_API_CreateIoCompletionPort  0x20B5u
#define WIN32_API_GetQueuedCompletionStatus 0x20B6u
#define WIN32_API_PostQueuedCompletionStatus 0x20B7u
#define WIN32_API_DeviceIoControl         0x20B8u
#define WIN32_API_FindFirstFileExA        0x20B9u
#define WIN32_API_FindFirstFileExW        0x20BAu
#define WIN32_API_FindFirstChangeNotificationA 0x20BBu
#define WIN32_API_FindFirstChangeNotificationW 0x20BCu
#define WIN32_API_FindNextChangeNotification 0x20BDu
#define WIN32_API_FindCloseChangeNotification 0x20BEu
#define WIN32_API_CreateDirectoryExA      0x20BFu
#define WIN32_API_CreateDirectoryExW      0x20C0u
#define WIN32_API_GetFileSecurityA        0x20C1u
#define WIN32_API_GetFileSecurityW        0x20C2u
#define WIN32_API_SetFileSecurityA        0x20C3u
#define WIN32_API_SetFileSecurityW        0x20C4u
#define WIN32_API_CompareFileTime         0x20C5u
#define WIN32_API_GetSystemDefaultLangID  0x20C6u
#define WIN32_API_GetUserDefaultLangID    0x20C7u
#define WIN32_API_GetSystemDefaultLCID    0x20C8u
#define WIN32_API_GetUserDefaultLCID      0x20C9u
#define WIN32_API_GetThreadLocale         0x20CAu
#define WIN32_API_SetThreadLocale         0x20CBu
#define WIN32_API_IsValidCodePage         0x20CCu
#define WIN32_API_GetCPInfoExA            0x20CDu
#define WIN32_API_GetCPInfoExW            0x20CEu
#define WIN32_API_GetNumberFormatA        0x20CFu
#define WIN32_API_GetNumberFormatW        0x20D0u
#define WIN32_API_GetDateFormatA          0x20D1u
#define WIN32_API_GetDateFormatW          0x20D2u
#define WIN32_API_GetTimeFormatA          0x20D3u
#define WIN32_API_GetTimeFormatW          0x20D4u
#define WIN32_API_MultiByteToWideChar     0x20D5u
#define WIN32_API_WideCharToMultiByte     0x20D6u
#define WIN32_API_CompareStringA          0x20D7u
#define WIN32_API_CompareStringW          0x20D8u
#define WIN32_API_LCMapStringA            0x20D9u
#define WIN32_API_LCMapStringW            0x20DAu
#define WIN32_API_GetStringTypeA          0x20DBu
#define WIN32_API_GetStringTypeW          0x20DCu
#define WIN32_API_CharUpperA              0x20DDu
#define WIN32_API_CharUpperW              0x20DEu
#define WIN32_API_CharLowerA              0x20DFu
#define WIN32_API_CharLowerW              0x20E0u
#define WIN32_API_GetLongPathNameA        0x20E1u
#define WIN32_API_GetLongPathNameW        0x20E2u
#define WIN32_API_GetShortPathNameA       0x20E3u
#define WIN32_API_GetShortPathNameW       0x20E4u
#define WIN32_API_CreateFileMappingA      0x20E5u
#define WIN32_API_CreateFileMappingW      0x20E6u
#define WIN32_API_OpenFileMappingA        0x20E7u
#define WIN32_API_OpenFileMappingW        0x20E8u
#define WIN32_API_MapViewOfFile           0x20E9u
#define WIN32_API_UnmapViewOfFile         0x20EAu
#define WIN32_API_FlushViewOfFile         0x20EBu
#define WIN32_API_CopyMemory              0x20ECu
#define WIN32_API_MoveMemory              0x20EDu
#define WIN32_API_FillMemory              0x20EEu
#define WIN32_API_ZeroMemory              0x20EFu

/* ============================================================
 * msvcrt
 * ============================================================ */
#define WIN32_API_memset                  0x20F0u
#define WIN32_API_memcpy                  0x20F1u
#define WIN32_API_memmove                 0x20F2u
#define WIN32_API_memcmp                  0x20F3u
#define WIN32_API_malloc                  0x20F4u
#define WIN32_API_calloc                  0x20F5u
#define WIN32_API_realloc                 0x20F6u
#define WIN32_API_free                    0x20F7u
#define WIN32_API_strlen                  0x20F8u
#define WIN32_API_strcmp                  0x20F9u
#define WIN32_API_strncmp                 0x20FAu
#define WIN32_API_strcpy                  0x20FBu
#define WIN32_API_strncpy                 0x20FCu
#define WIN32_API_strcat                  0x20FDu
#define WIN32_API_strchr                  0x20FEu
#define WIN32_API_strrchr                 0x20FFu
#define WIN32_API_strstr                  0x2100u
#define WIN32_API_atoi                    0x2101u
#define WIN32_API_atol                    0x2102u
#define WIN32_API_itoa                    0x2103u
#define WIN32_API__itoa                   0x2104u
#define WIN32_API__itoa_s                 0x2105u
#define WIN32_API__ltoa                   0x2106u
#define WIN32_API_strdup                  0x2107u
#define WIN32_API__strdup                 0x2108u
#define WIN32_API_stricmp                 0x2109u
#define WIN32_API__stricmp                0x210Au
#define WIN32_API_printf                  0x210Bu
#define WIN32_API_sprintf                 0x210Cu
#define WIN32_API_snprintf                0x210Du
#define WIN32_API_vprintf                 0x210Eu
#define WIN32_API_vsnprintf               0x210Fu
#define WIN32_API_puts                    0x2110u
#define WIN32_API_putchar                 0x2111u
#define WIN32_API_getchar                 0x2112u
#define WIN32_API_fopen                   0x2113u
#define WIN32_API_fclose                  0x2114u
#define WIN32_API_fread                   0x2115u
#define WIN32_API_fwrite                  0x2116u
#define WIN32_API_fseek                   0x2117u
#define WIN32_API_ftell                   0x2118u
#define WIN32_API_fflush                  0x2119u
#define WIN32_API__snprintf               0x211Au
#define WIN32_API__vsnprintf              0x211Bu
#define WIN32_API_abort                   0x211Cu
#define WIN32_API_exit                    0x211Du
#define WIN32_API__exit                   0x211Eu
#define WIN32_API_getenv                  0x211Fu
#define WIN32_API_system                  0x2120u
#define WIN32_API_rand                    0x2121u
#define WIN32_API_srand                   0x2122u
#define WIN32_API_qsort                   0x2123u
#define WIN32_API_bsearch                 0x2124u
#define WIN32_API__beginthreadex          0x2125u
#define WIN32_API__endthreadex            0x2126u
#define WIN32_API_time                    0x2127u

/* ============================================================
 * advapi32
 * ============================================================ */
#define WIN32_API_RegOpenKeyExA           0x2128u
#define WIN32_API_RegOpenKeyExW           0x2129u
#define WIN32_API_RegQueryValueExA        0x212Au
#define WIN32_API_RegQueryValueExW        0x212Bu
#define WIN32_API_RegSetValueExA          0x212Cu
#define WIN32_API_RegSetValueExW          0x212Du
#define WIN32_API_RegCloseKey             0x212Eu
#define WIN32_API_RegCreateKeyExA         0x212Fu
#define WIN32_API_RegCreateKeyExW         0x2130u
#define WIN32_API_RegDeleteKeyA           0x2131u
#define WIN32_API_RegDeleteKeyW           0x2132u
#define WIN32_API_RegDeleteValueA         0x2133u
#define WIN32_API_RegDeleteValueW         0x2134u
#define WIN32_API_RegEnumKeyExA           0x2135u
#define WIN32_API_RegEnumKeyExW           0x2136u
#define WIN32_API_RegEnumValueA           0x2137u
#define WIN32_API_RegEnumValueW           0x2138u
#define WIN32_API_RegQueryInfoKeyA        0x2139u
#define WIN32_API_RegQueryInfoKeyW        0x213Au
#define WIN32_API_OpenProcessToken        0x213Bu
#define WIN32_API_OpenThreadToken         0x213Cu
#define WIN32_API_LookupPrivilegeValueA   0x213Du
#define WIN32_API_LookupPrivilegeValueW   0x213Eu
#define WIN32_API_AdjustTokenPrivileges   0x213Fu
#define WIN32_API_GetTokenInformation     0x2140u
#define WIN32_API_AllocateAndInitializeSid 0x2141u
#define WIN32_API_FreeSid                 0x2142u

/* ============================================================
 * user32
 * ============================================================ */
#define WIN32_API_MessageBoxA             0x2143u
#define WIN32_API_MessageBoxW             0x2144u
#define WIN32_API_CreateWindowExA         0x2145u
#define WIN32_API_CreateWindowExW         0x2146u
#define WIN32_API_DefWindowProcA          0x2147u
#define WIN32_API_DefWindowProcW          0x2148u
#define WIN32_API_GetMessageA             0x2149u
#define WIN32_API_GetMessageW             0x214Au
#define WIN32_API_TranslateMessage        0x214Bu
#define WIN32_API_DispatchMessageA        0x214Cu
#define WIN32_API_DispatchMessageW        0x214Du
#define WIN32_API_PostQuitMessage         0x214Eu
#define WIN32_API_ShowWindow              0x214Fu
#define WIN32_API_UpdateWindow            0x2150u
#define WIN32_API_GetClientRect           0x2151u
#define WIN32_API_BeginPaint              0x2152u
#define WIN32_API_EndPaint                0x2153u
#define WIN32_API_LoadIconA               0x2154u
#define WIN32_API_LoadIconW               0x2155u
#define WIN32_API_LoadCursorA             0x2156u
#define WIN32_API_LoadCursorW             0x2157u
#define WIN32_API_RegisterClassA          0x2158u
#define WIN32_API_RegisterClassW          0x2159u
#define WIN32_API_SetWindowTextA          0x215Au
#define WIN32_API_SetWindowTextW          0x215Bu
#define WIN32_API_GetWindowTextA          0x215Cu
#define WIN32_API_GetWindowTextW          0x215Du
#define WIN32_API_GetDesktopWindow        0x215Eu
#define WIN32_API_GetForegroundWindow     0x215Fu
#define WIN32_API_SetForegroundWindow     0x2160u
#define WIN32_API_FindWindowA             0x2161u
#define WIN32_API_FindWindowW             0x2162u
#define WIN32_API_SendMessageA            0x2163u
#define WIN32_API_SendMessageW            0x2164u
#define WIN32_API_PostMessageA            0x2165u
#define WIN32_API_PostMessageW            0x2166u
#define WIN32_API_RegisterClassExA        0x2167u
#define WIN32_API_RegisterClassExW        0x2168u
#define WIN32_API_UnregisterClassA        0x2169u
#define WIN32_API_UnregisterClassW        0x216Au
#define WIN32_API_CreateWindowA           0x216Bu
#define WIN32_API_CreateWindowW           0x216Cu
#define WIN32_API_DestroyWindow           0x216Du
#define WIN32_API_GetParent               0x216Eu
#define WIN32_API_SetParent               0x216Fu
#define WIN32_API_IsWindow                0x2170u
#define WIN32_API_IsWindowVisible         0x2171u
#define WIN32_API_EnableWindow            0x2172u
#define WIN32_API_GetSystemMetrics        0x2173u
#define WIN32_API_GetWindowRect           0x2174u
#define WIN32_API_MoveWindow              0x2175u
#define WIN32_API_SetWindowPos            0x2176u
#define WIN32_API_GetWindowLongA          0x2177u
#define WIN32_API_SetWindowLongA          0x2178u

/* ============================================================
 * gdi32
 * ============================================================ */
#define WIN32_API_GetStockObject          0x2179u
#define WIN32_API_CreateSolidBrush        0x217Au
#define WIN32_API_CreatePen               0x217Bu
#define WIN32_API_SelectObject            0x217Cu
#define WIN32_API_DeleteObject            0x217Du
#define WIN32_API_TextOutA                0x217Eu
#define WIN32_API_TextOutW                0x217Fu
#define WIN32_API_BitBlt                  0x2180u
#define WIN32_API_StretchBlt              0x2181u
#define WIN32_API_SetBkMode               0x2182u
#define WIN32_API_SetTextColor            0x2183u
#define WIN32_API_SetBkColor              0x2184u
#define WIN32_API_CreateCompatibleDC      0x2185u
#define WIN32_API_CreateCompatibleBitmap  0x2186u
#define WIN32_API_DeleteDC                0x2187u
#define WIN32_API_GetDeviceCaps           0x2188u
#define WIN32_API_Rectangle               0x2189u
#define WIN32_API_Ellipse                 0x218Au
#define WIN32_API_MoveToEx                0x218Bu
#define WIN32_API_LineTo                  0x218Cu

/* ============================================================
 * shell32
 * ============================================================ */
#define WIN32_API_ShellExecuteA           0x218Du
#define WIN32_API_ShellExecuteW           0x218Eu
#define WIN32_API_ShellExecuteExA         0x218Fu
#define WIN32_API_ShellExecuteExW         0x2190u
#define WIN32_API_SHGetFolderPathA        0x2191u
#define WIN32_API_SHGetFolderPathW        0x2192u
#define WIN32_API_SHGetKnownFolderPath    0x2193u
#define WIN32_API_SHGetSpecialFolderPathA 0x2194u
#define WIN32_API_SHGetSpecialFolderPathW 0x2195u

/* ============================================================
 * ole32
 * ============================================================ */
#define WIN32_API_CoInitialize            0x2196u
#define WIN32_API_CoUninitialize          0x2197u
#define WIN32_API_CoCreateInstance        0x2198u
#define WIN32_API_CoTaskMemAlloc          0x2199u
#define WIN32_API_CoTaskMemFree           0x219Au
#define WIN32_API_CoInitializeEx          0x219Bu

/* ============================================================
 * ws2_32
 * ============================================================ */
#define WIN32_API_WSAStartup              0x219Cu
#define WIN32_API_WSACleanup              0x219Du
#define WIN32_API_socket                 0x219Eu
#define WIN32_API_bind                   0x219Fu
#define WIN32_API_listen                 0x21A0u
#define WIN32_API_accept                 0x21A1u
#define WIN32_API_connect                0x21A2u
#define WIN32_API_send                   0x21A3u
#define WIN32_API_recv                   0x21A4u
#define WIN32_API_closesocket            0x21A5u
#define WIN32_API_gethostbyname          0x21A6u
#define WIN32_API_getaddrinfo            0x21A7u
#define WIN32_API_freeaddrinfo           0x21A8u
#define WIN32_API_inet_addr              0x21A9u
#define WIN32_API_inet_ntoa              0x21AAu
#define WIN32_API_htons                  0x21ABu
#define WIN32_API_htonl                  0x21ACu
#define WIN32_API_ntohs                  0x21ADu
#define WIN32_API_ntohl                  0x21AEu
#define WIN32_API_WSAGetLastError        0x21AFu
#define WIN32_API_WSASetLastError        0x21B0u
#define WIN32_API_select                 0x21B1u
#define WIN32_API_ioctlsocket            0x21B2u
#define WIN32_API_setsockopt             0x21B3u
#define WIN32_API_getsockopt             0x21B4u
#define WIN32_API_shutdown               0x21B5u
#define WIN32_API_recvfrom               0x21B6u
#define WIN32_API_sendto                 0x21B7u

/* ============================================================
 * winmm
 * ============================================================ */
#define WIN32_API_timeGetTime             0x21B8u
#define WIN32_API_timeBeginPeriod         0x21B9u
#define WIN32_API_timeEndPeriod           0x21BAu
#define WIN32_API_PlaySoundA              0x21BBu
#define WIN32_API_PlaySoundW              0x21BCu

/* ============================================================
 * comctl32
 * ============================================================ */
#define WIN32_API_InitCommonControls      0x21BDu
#define WIN32_API_InitCommonControlsEx    0x21BEu
#define WIN32_API_ImageList_Create        0x21BFu

/* ============================================================
 * ntdll
 * ============================================================ */
#define WIN32_API_NtCreateFile            0x21C0u
#define WIN32_API_NtOpenFile              0x21C1u
#define WIN32_API_NtReadFile              0x21C2u
#define WIN32_API_NtWriteFile             0x21C3u
#define WIN32_API_NtClose                 0x21C4u
#define WIN32_API_NtCreateProcess         0x21C5u
#define WIN32_API_NtCreateThread          0x21C6u
#define WIN32_API_NtAllocateVirtualMemory 0x21C7u
#define WIN32_API_NtFreeVirtualMemory     0x21C8u
#define WIN32_API_NtQueryInformationProcess 0x21C9u
#define WIN32_API_NtSetInformationProcess 0x21CAu
#define WIN32_API_NtQueryInformationThread 0x21CBu
#define WIN32_API_NtSetInformationThread  0x21CCu
#define WIN32_API_NtWaitForSingleObject   0x21CDu
#define WIN32_API_NtWaitForMultipleObjects 0x21CEu
#define WIN32_API_NtDelayExecution        0x21CFu
#define WIN32_API_NtQuerySystemInformation 0x21D0u
#define WIN32_API_NtQueryPerformanceCounter 0x21D1u
#define WIN32_API_NtQueryVirtualMemory    0x21D2u
#define WIN32_API_NtProtectVirtualMemory  0x21D3u
#define WIN32_API_NtCreateKey             0x21D4u
#define WIN32_API_NtOpenKey               0x21D5u
#define WIN32_API_NtQueryValueKey         0x21D6u
#define WIN32_API_NtSetValueKey           0x21D7u
#define WIN32_API_NtDeleteKey             0x21D8u
#define WIN32_API_NtEnumerateKey          0x21D9u
#define WIN32_API_NtEnumerateValueKey     0x21DAu
#define WIN32_API_NtQueryKey              0x21DBu
#define WIN32_API_NtFlushKey              0x21DCu
#define WIN32_API_RtlInitUnicodeString    0x21DDu

/* ============================================================
 * userenv
 * ============================================================ */
#define WIN32_API_GetUserProfileDirectoryA 0x21DEu
#define WIN32_API_GetUserProfileDirectoryW 0x21DFu
#define WIN32_API_GetProfilesDirectoryA    0x21E0u

/* ============================================================
 * msvcrt（附加）
 * ============================================================ */
#define WIN32_API_memchr                  0x21E1u
#define WIN32_API_strncat                 0x21E2u
#define WIN32_API_strspn                 0x21E3u
#define WIN32_API_strcspn                 0x21E4u
#define WIN32_API_strpbrk                 0x21E5u
#define WIN32_API_strtok                  0x21E6u
#define WIN32_API_toupper                 0x21E7u

/* ============================================================
 * kernel32（附加 13 项，补齐 500）
 * ============================================================ */
#define WIN32_API_GetLastError_Compat     0x21E8u
#define WIN32_API_SetConsoleTitleA        0x21E9u
#define WIN32_API_GetConsoleTitleA        0x21EAu
#define WIN32_API_SetConsoleWindowSize    0x21EBu
#define WIN32_API_GetCurrentConsoleFont   0x21ECu
#define WIN32_API_GetProcessVersion       0x21EDu
#define WIN32_API_GetProcessAffinityMask  0x21EEu
#define WIN32_API_SetThreadAffinityMask   0x21EFu
#define WIN32_API_SwitchToThread          0x21F0u
#define WIN32_API_GetThreadContext        0x21F1u
#define WIN32_API_SetThreadContext        0x21F2u
#define WIN32_API_FlushInstructionCache   0x21F3u
#define WIN32_API_GetCurrentProcessorNumber 0x21F4u

/* ============================================================
 * Windows 错误码
 * ============================================================ */
#define WIN32_ERROR_SUCCESS              0u
#define WIN32_ERROR_INVALID_FUNCTION     1u
#define WIN32_ERROR_FILE_NOT_FOUND       2u
#define WIN32_ERROR_PATH_NOT_FOUND       3u
#define WIN32_ERROR_ACCESS_DENIED        5u
#define WIN32_ERROR_INVALID_HANDLE       6u
#define WIN32_ERROR_NOT_ENOUGH_MEMORY    8u
#define WIN32_ERROR_INVALID_DATA         13u
#define WIN32_ERROR_OUTOFMEMORY          14u
#define WIN32_ERROR_INVALID_PARAMETER    87u
#define WIN32_ERROR_BROKEN_PIPE          109u
#define WIN32_ERROR_DISK_FULL            112u
#define WIN32_ERROR_CALL_NOT_IMPLEMENTED 120u
#define WIN32_ERROR_INSUFFICIENT_BUFFER  122u
#define WIN32_ERROR_ALREADY_EXISTS       183u

/* ---------- 句柄约定 ---------- */
#define WIN32_STD_INPUT_HANDLE   ((uint64_t)-10)
#define WIN32_STD_OUTPUT_HANDLE  ((uint64_t)-11)
#define WIN32_STD_ERROR_HANDLE   ((uint64_t)-12)
#define WIN32_INVALID_HANDLE     ((uint64_t)-1)

/* ---------- 用户态 stub 基址 ---------- */
#define WIN32_STUB_BASE      0x000000A000000000ULL
#define WIN32_STUB_STRIDE    16ULL
#define WIN32_STUB_PAGE_SIZE 0x100000ULL

/* ---------- TEB / PEB 地址 ---------- */
#define WIN32_TEB_VA         0x00007FFFFFE00000ULL
#define WIN32_PEB_VA         0x00007FFFFFE01000ULL

/* ---------- 接口 ---------- */
void win32_api_init(void);
int64_t win32_api_dispatch(struct task_t *cur, struct syscall_frame *f);

/*
 * 名称规范化 + 查表。命中返回 1 并写 *out_nr；未命中返回 0。
 * 规范化规则：
 *   - dll 大小写不敏感，去掉可选 ".dll"
 *   - func 大小写不敏感，去掉前导 '_'，去掉尾部 '@N'
 */
int win32_api_lookup(const char *dll, const char *func, uint32_t *out_nr);

/* ---------- 表驱动分发器的“实现表”查询（供分发器判断是否已实现） ---------- */
typedef int64_t (*win32_api_fn)(struct task_t *cur, struct syscall_frame *f);

int win32_api_has_impl(uint32_t api_nr, win32_api_fn *out_fn);

/* ---------- PEB/TEB ---------- */
int win32_setup_peb_teb(struct task_t *t, uint64_t image_base,
                        const char *cmdline);
uint64_t win32_peb_va(struct task_t *t);
uint64_t win32_teb_va(struct task_t *t);

/* ---------- stub ---------- */
int win32_stub_init(struct task_t *t, uint64_t stub_base);
uint64_t win32_stub_va(struct task_t *t, uint32_t api_nr);
uint64_t win32_stub_page_bytes(void);

#endif /* OMNIBRIDGE_USER_WIN32_API_H */
/*===OmniBridgeOs/kernel/arch/x64/user/win32_api.h 结束===*/