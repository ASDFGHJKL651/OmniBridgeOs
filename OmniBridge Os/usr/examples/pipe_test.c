/*===OmniBridgeOs/usr/examples/pipe_test.c===*/
#include "../include/ob/ob.h"
#include "../include/ob/stdio.h"
#include "../include/ob/unistd.h"
#include "../include/ob/string.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    printf("pipe_test start\n");

    int fds[2];
    if (pipe(fds) != 0) {
        printf("[pipe_test] FAIL: pipe() failed\n");
        return 1;
    }
    printf("[pipe_test] pipe created: rd=%d wr=%d\n", fds[0], fds[1]);

    const char *msg = "hello pipe";
    int64_t nw = write(fds[1], msg, strlen(msg));
    if (nw != (int64_t)strlen(msg)) {
        printf("[pipe_test] FAIL: write returned %lld\n", (long long)nw);
        return 1;
    }

    char buf[32];
    int64_t nr = read(fds[0], buf, sizeof(buf) - 1);
    if (nr != nw) {
        printf("[pipe_test] FAIL: read returned %lld\n", (long long)nr);
        return 1;
    }
    buf[nr] = '\0';

    if (strcmp(buf, msg) != 0) {
        printf("[pipe_test] FAIL: content mismatch '%s'\n", buf);
        return 1;
    }

    printf("[pipe_test] OK\n");
    return 0;
}