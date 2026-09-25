/*===OmniBridgeOs/usr/examples/login_test.c===*/
#include "../include/ob/ob.h"
#include "../include/ob/stdio.h"
#include "../include/ob/stdlib.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    printf("=== OmniBridge login test ===\n");

    /* 模拟登录：向内核发送 SYS_OB_CheckAccess 验证凭据。
     * 本演示仅显示；实际登录由 OShell 集成。 */
    printf("Try: login root / login user\n");
    printf("Then: whoami, su user\n");
    printf("login_test done\n");
    return 0;
}
/*===OmniBridgeOs/usr/examples/login_test.c 结束===*/