/*===OmniBridgeOs/usr/include/ob/stdio.h===*/
#ifndef OB_USER_STDIO_H
#define OB_USER_STDIO_H

#include <stddef.h>
#include <stdarg.h>

#define EOF     (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct ob_FILE ob_FILE;

extern ob_FILE *stdin;
extern ob_FILE *stdout;
extern ob_FILE *stderr;

int    printf(const char *fmt, ...);
int    fprintf(ob_FILE *f, const char *fmt, ...);
int    sprintf(char *buf, const char *fmt, ...);
int    snprintf(char *buf, size_t n, const char *fmt, ...);
int    vprintf(const char *fmt, va_list ap);
int    vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

int    puts(const char *s);
int    putchar(int c);
int    getchar(void);

ob_FILE *fopen(const char *path, const char *mode);
int     fclose(ob_FILE *f);
size_t  fread(void *buf, size_t sz, size_t n, ob_FILE *f);
size_t  fwrite(const void *buf, size_t sz, size_t n, ob_FILE *f);
int     fseek(ob_FILE *f, long off, int whence);
long    ftell(ob_FILE *f);
int     fflush(ob_FILE *f);

#endif /* OB_USER_STDIO_H */
/*===OmniBridgeOs/usr/include/ob/stdio.h 结束===*/