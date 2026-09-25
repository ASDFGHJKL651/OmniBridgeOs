/*===OmniBridgeOs/usr/include/ob/stdlib.h===*/
#ifndef OB_USER_STDLIB_H
#define OB_USER_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void  *malloc(size_t size);
void  *calloc(size_t n, size_t size);
void  *realloc(void *p, size_t size);
void   free(void *p);

int    atoi(const char *s);
long   atol(const char *s);
char  *itoa(int v, char *buf, int base);

void   exit(int code) __attribute__((noreturn));
void   abort(void) __attribute__((noreturn));

#endif /* OB_USER_STDLIB_H */
/*===OmniBridgeOs/usr/include/ob/stdlib.h 结束===*/