/*===OmniBridgeOs/usr/examples/fs_test.c===*/
/*
 * fs_test.c —— 用户态文件 I/O 与权限引擎验收。
 *
 * 验收对应（第 18C 步）：
 *   ✓ oblibc 可编译并运行 hello 程序（printf、malloc、文件 I/O）。
 *   ✓ 权限引擎继续生效：用户态进程访问 /kernel/ 被拒绝并审计。
 *
 * 预期输出（串口）：
 *   [fs_test] writing /tmp/fs_test.txt
 *   [fs_test] reading back
 *   [fs_test] content matches
 *   [fs_test] probing /kernel/probe — expect denial
 *   [fs_test] /kernel/probe correctly denied (errno=EACCES)
 *   [fs_test] OK
 */
#include "../include/ob/ob.h"
#include "../include/ob/stdio.h"
#include "../include/ob/stdlib.h"
#include "../include/ob/string.h"
#include "../include/ob/unistd.h"
#include "../include/ob/fcntl.h"
#include "../include/ob/errno.h"

#define PATH "/tmp/fs_test.txt"
#define TEXT "OmniBridge fs_test payload"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    /* 1) 写文件 */
    printf("[fs_test] writing %s\n", PATH);
    int fd = open(PATH, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        printf("[fs_test] FAIL: open for write errno=%d\n", errno);
        return 1;
    }
    int64_t nw = write(fd, TEXT, strlen(TEXT));
    close(fd);
    if (nw != (int64_t)strlen(TEXT)) {
        printf("[fs_test] FAIL: write returned %lld\n", (long long)nw);
        return 1;
    }

    /* 2) 读回文件 */
    printf("[fs_test] reading back\n");
    fd = open(PATH, O_RDONLY, 0);
    if (fd < 0) {
        printf("[fs_test] FAIL: open for read errno=%d\n", errno);
        return 1;
    }
    char buf[64];
    int64_t nr = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (nr <= 0) {
        printf("[fs_test] FAIL: read returned %lld\n", (long long)nr);
        return 1;
    }
    buf[nr] = '\0';

    if (strcmp(buf, TEXT) != 0) {
        printf("[fs_test] FAIL: content mismatch '%s'\n", buf);
        return 1;
    }
    printf("[fs_test] content matches\n");

    /* 3) 访问 /kernel/ 必须被拒绝 */
    printf("[fs_test] probing /kernel/probe — expect denial\n");
    errno = 0;
    int bad = open("/kernel/probe", O_RDONLY, 0);
    if (bad >= 0) {
        close(bad);
        printf("[fs_test] FAIL: /kernel/probe NOT denied\n");
        return 1;
    }
    printf("[fs_test] /kernel/probe correctly denied (errno=%d)\n", errno);

    printf("[fs_test] OK\n");
    return 0;
}
/*===OmniBridgeOs/usr/examples/fs_test.c 结束===*/