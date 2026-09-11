#include <stdio.h>
#include <string.h>
#include "serial.h"

static int failures = 0;

#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr);     \
            failures++;                                                 \
        }                                                               \
    } while (0)

int main(void)
{
    char buf[128];
    int n;

    n = ob_snprintf(buf, sizeof(buf), "Knot %s %d %u %x", "booting", -42, 42u, 0xABCu);
    printf("ob_snprintf -> '%s' (n=%d)\n", buf, n);
    CHECK(strcmp(buf, "Knot booting -42 42 abc") == 0);
    CHECK(n == (int)strlen("Knot booting -42 42 abc"));

    ob_snprintf(buf, 4, "abcdef");
    CHECK(strcmp(buf, "abc") == 0);

    ob_snprintf(buf, sizeof(buf), "%p", (void *)0x1234);
    CHECK(strcmp(buf, "0x1234") == 0);

    ob_snprintf(buf, sizeof(buf), "%llu", 1234567890123ULL);
    CHECK(strcmp(buf, "1234567890123") == 0);

    if (failures == 0) {
        printf("All host tests passed.\n");
        return 0;
    }

    printf("%d host test(s) failed.\n", failures);
    return 1;
}