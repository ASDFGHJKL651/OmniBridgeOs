/*===OmniBridgeOs/usr/examples/hello.c===*/
#include "../include/ob/ob.h"
#include "../include/ob/stdio.h"
#include "../include/ob/stdlib.h"
#include "../include/ob/string.h"

int main(int argc, char **argv, char **envp)
{
    (void)envp;
    printf("Hello from user mode! argc=%d\n", argc);
    for (int i = 0; i < argc; ++i)
        printf("  argv[%d] = %s\n", i, argv[i]);

    char *p = (char *)malloc(64);
    if (p) {
        strcpy(p, "malloc works");
        printf("%s\n", p);
        free(p);
    }
    return 0;
}
/*===OmniBridgeOs/usr/examples/hello.c 结束===*/