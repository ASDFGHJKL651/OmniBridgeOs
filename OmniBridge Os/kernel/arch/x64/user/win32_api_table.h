/*===OmniBridgeOs/kernel/arch/x64/user/win32_api_table.h===*/
/*
 * Win32 API 名称表 —— 第 20 步扩展：500 项。
 *
 * 人工必须审查：
 *   - 每项的 (dll, func, nr) 必须唯一且 nr 落在
 *     [WIN32_API_FIRST, WIN32_API_LAST]。
 *   - dll 使用小写、不带 .dll。
 *   - func 使用规范名，不带前导 '_'，不带 @N 装饰。
 *   - 表项顺序不限定；分发器查表时线性扫描（500 项足够快）。
 */
#ifndef OMNIBRIDGE_USER_WIN32_API_TABLE_H
#define OMNIBRIDGE_USER_WIN32_API_TABLE_H

#include <stdint.h>
#include "win32_api.h"

struct win32_api_entry {
    const char *dll;
    const char *func;
    uint32_t    nr;
};

extern const struct win32_api_entry g_win32_api_table[];
extern const uint32_t              g_win32_api_table_count;

uint32_t win32_api_table_count(void);

#endif /* OMNIBRIDGE_USER_WIN32_API_TABLE_H */
/*===OmniBridgeOs/kernel/arch/x64/user/win32_api_table.h 结束===*/