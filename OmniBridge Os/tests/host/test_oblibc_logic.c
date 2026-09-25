/*===OmniBridgeOs/tests/host/test_oblibc_logic.c===*/
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int fail = 0;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fail++; } } while (0)

/* 直接从 oblibc 中拷贝的纯逻辑函数（不依赖 syscall） */
static size_t my_strlen(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
static int    my_strcmp(const char *a, const char *b)
{ while (*a && *a == *b) { ++a; ++b; } return (unsigned char)*a - (unsigned char)*b; }
static char  *my_strchr(const char *s, int c)
{ while (*s) { if (*s == (char)c) return (char *)s; ++s; } return 0; }

int main(void)
{
    CHECK(my_strlen("") == 0);
    CHECK(my_strlen("hello") == 5);
    CHECK(my_strcmp("abc", "abc") == 0);
    CHECK(my_strcmp("abc", "abd") < 0);
    CHECK(my_strchr("hello", 'l') != 0);
    CHECK(my_strchr("hello", 'z') == 0);

    if (fail == 0) { printf("test_oblibc_logic: OK\n"); return 0; }
    printf("test_oblibc_logic: %d failures\n", fail);
    return 1;
}
/*===OmniBridgeOs/tests/host/test_oblibc_logic.c 结束===*/