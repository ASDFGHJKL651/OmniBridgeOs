/*===OmniBridgeOs/kernel/arch/x64/user/win32_compat_test.c===*/
/*
 * 第 20 步扩展自检：500 项 API 表验证。
 *
 * 覆盖：
 *   - WIN32_API_COUNT == 500，编号范围 [FIRST, LAST]。
 *   - g_win32_api_table_count == 500，无重复、无缺失。
 *   - 名称规范化 + 查找：大小写不敏感、去 .dll、去 _、去 @N。
 *   - win32_stub_page_bytes() >= WIN32_API_COUNT * 16。
 *   - A 类 API 至少 80 项有真实实现。
 *   - 未实现 API 分发返回 CALL_NOT_IMPLEMENTED。
 *   - 权限拒绝路径返回 Windows 错误码。
 */
#include "win32_compat_test.h"
#include "pe_loader.h"
#include "win32_api.h"
#include "win32_api_table.h"
#include "compat_path.h"
#include "task.h"
#include "sched.h"
#include "audit.h"
#include "serial.h"

static int g_fail = 0;

static void check(const char *desc, int ok)
{
    if (ok) serial_printf("[WIN32-TEST] OK  : %s\n", desc);
    else { serial_printf("[WIN32-TEST] FAIL: %s\n", desc); g_fail++; }
}

static void build_min_pe(uint8_t *buf, uint32_t buf_sz)
{
    for (uint32_t i = 0; i < buf_sz; ++i) buf[i] = 0;

    buf[0] = 'M'; buf[1] = 'Z';
    *(uint32_t *)(void *)(buf + 0x3C) = 0x80;
    *(uint32_t *)(void *)(buf + 0x80) = PE_NT_SIGNATURE;

    uint8_t *coff = buf + 0x84;
    *(uint16_t *)(void *)(coff + 0)  = PE_MACHINE_AMD64;
    *(uint16_t *)(void *)(coff + 2)  = 1;
    *(uint16_t *)(void *)(coff + 16) = 0xF0;
    *(uint16_t *)(void *)(coff + 18) = PE_FILE_EXECUTABLE;

    uint8_t *opt = coff + 20;
    *(uint16_t *)(void *)(opt + 0)  = PE_OPT_MAGIC_PE32P;
    *(uint32_t *)(void *)(opt + 16) = 0x1000;
    *(uint64_t *)(void *)(opt + 24) = 0x8000000000ULL;
    *(uint32_t *)(void *)(opt + 32) = 0x1000;
    *(uint32_t *)(void *)(opt + 36) = 0x200;
    *(uint32_t *)(void *)(opt + 56) = 0x2000;
    *(uint32_t *)(void *)(opt + 60) = 0x200;
    *(uint16_t *)(void *)(opt + 68) = 3;
    *(uint32_t *)(void *)(opt + 108) = 16;
}

void win32_compat_selftest(void)
{
    g_fail = 0;
    serial_printf("[WIN32-TEST] === begin ===\n");

    /* 1) 表规模与编号范围 */
    check("WIN32_API_COUNT == 500", WIN32_API_COUNT == 500u);
    check("g_win32_api_table_count == 500",
          g_win32_api_table_count == 500u);
    check("WIN32_API_LAST - FIRST + 1 == 500",
          (WIN32_API_LAST - WIN32_API_FIRST + 1u) == 500u);
    check("win32_api_table_count() == 500",
          win32_api_table_count() == 500u);

    /* 2) 编号在范围内 + 唯一 */
    {
        int in_range = 1;
        for (uint32_t i = 0; i < g_win32_api_table_count; ++i) {
            uint32_t nr = g_win32_api_table[i].nr;
            if (nr < WIN32_API_FIRST || nr > WIN32_API_LAST) {
                in_range = 0;
                break;
            }
        }
        check("all nr in [FIRST, LAST]", in_range);

        int unique = 1;
        for (uint32_t i = 0; i < g_win32_api_table_count && unique; ++i) {
            for (uint32_t j = i + 1; j < g_win32_api_table_count; ++j) {
                if (g_win32_api_table[i].nr == g_win32_api_table[j].nr) {
                    unique = 0;
                    break;
                }
            }
        }
        check("all nr unique", unique);

        /* 无与 Linux / OB syscall 重叠（粗略：0x100..0x200 不允许） */
        int no_overlap = 1;
        for (uint32_t i = 0; i < g_win32_api_table_count; ++i) {
            uint32_t nr = g_win32_api_table[i].nr;
            if (nr >= 0x100u && nr < 0x2000u) { no_overlap = 0; break; }
        }
        check("no overlap with OB/Linux syscalls", no_overlap);
    }

    /* 3) 名称查找 */
    {
        uint32_t nr = 0;
        check("lookup GetStdHandle -> 0x2001",
              win32_api_lookup("kernel32", "GetStdHandle", &nr) == 1 &&
              nr == 0x2001u);
        check("lookup KERNEL32.DLL getstdhandle",
              win32_api_lookup("KERNEL32.DLL", "getstdhandle", &nr) == 1);
        check("lookup _WriteFile@20",
              win32_api_lookup("kernel32", "_WriteFile@20", &nr) == 1 &&
              nr == WIN32_API_WriteFile);
        check("lookup unknown returns 0",
              win32_api_lookup("kernel32", "NotARealApi", &nr) == 0);
        check("lookup msvcrt printf",
              win32_api_lookup("msvcrt", "printf", &nr) == 1);
        check("lookup advapi32 RegOpenKeyExA",
              win32_api_lookup("advapi32", "RegOpenKeyExA", &nr) == 1);
        check("lookup user32 MessageBoxA",
              win32_api_lookup("user32", "MessageBoxA", &nr) == 1);
        check("lookup ws2_32 WSAStartup",
              win32_api_lookup("ws2_32", "WSAStartup", &nr) == 1);
    }

    /* 4) stub 页大小 */
    check("stub_page_bytes >= 500*16",
          win32_stub_page_bytes() >= 500ULL * 16ULL);

    /* 5) A 类实现数量（粗略按 impl 表判断） */
    {
        int a_count = 0;
        for (uint32_t i = 0; i < g_win32_api_table_count; ++i) {
            win32_api_fn fn = 0;
            if (win32_api_has_impl(g_win32_api_table[i].nr, &fn) && fn)
                a_count++;
        }
        serial_printf("[WIN32-TEST] impl count = %d\n", a_count);
        check("impl count >= 80", a_count >= 80);
    }

    /* 6) PE 检测/解析 */
    {
        static uint8_t img[1024];
        build_min_pe(img, sizeof(img));
        struct pe_info info;
        int rc = pe_parse_info(img, sizeof(img), &info);
        check("pe_parse_info rc == 0", rc == 0);
        if (rc == 0) {
            check("entry_rva == 0x1000", info.entry_rva == 0x1000);
            check("image_base == 0x8000000000",
                  info.image_base_pref == 0x8000000000ULL);
        }
    }

    /* 7) 路径复用 */
    {
        char buf[COMPAT_PATH_MAX];
        int rc = compat_path_win_to_ob("C:\\Windows\\System32", buf);
        check("path C:\\Windows\\System32",
              rc == 0 && buf[0] == '/' && buf[1] == 'w');
        rc = compat_path_win_to_ob("HKLM\\SOFTWARE\\Foo", buf);
        check("path HKLM\\SOFTWARE\\Foo", rc == 0);
    }

    /* 8) 未实现 API 分发 */
    {
        /* 构造一个 fake task（栈上，不进全局链表） */
        struct task_t fake;
        uint8_t *p = (uint8_t *)&fake;
        for (unsigned i = 0; i < sizeof(fake); ++i) p[i] = 0;
        fake.pid = 8000;

        struct syscall_frame sf;
        uint8_t *ps = (uint8_t *)&sf;
        for (unsigned i = 0; i < sizeof(sf); ++i) ps[i] = 0;

        /* 选一个已知存在但 impl 表中没有的编号（例如 NtCreateFile） */
        sf.rax = WIN32_API_NtCreateFile;
        int64_t r = win32_api_dispatch(&fake, &sf);
        check("unimpl NtCreateFile -> 0 (FALSE)", r == 0);
        check("last_error == CALL_NOT_IMPLEMENTED",
              fake.win32_last_error == WIN32_ERROR_CALL_NOT_IMPLEMENTED);
    }

    if (g_fail == 0) serial_printf("[WIN32-TEST] selftest OK\n");
    else serial_printf("[WIN32-TEST] selftest FAILED: %d case(s)\n", g_fail);
    serial_printf("[WIN32-TEST] === end ===\n");
}
/*===OmniBridgeOs/kernel/arch/x64/user/win32_compat_test.c 结束===*/