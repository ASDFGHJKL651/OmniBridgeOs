/*===OmniBridgeOs/usr/lib/oblibc/string.c===*/
#include "../../include/ob/string.h"

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *dd = d; const unsigned char *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *dd = d; const unsigned char *ss = s;
    if (dd < ss) { while (n--) *dd++ = *ss++; }
    else { dd += n; ss += n; while (n--) *--dd = *--ss; }
    return d;
}

void *memset(void *d, int c, size_t n)
{
    unsigned char *dd = d;
    while (n--) *dd++ = (unsigned char)c;
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    while (n--) { if (*x != *y) return (int)*x - (int)*y; ++x; ++y; }
    return 0;
}

size_t strlen(const char *s)
{ size_t n = 0; while (s[n]) ++n; return n; }

int strcmp(const char *a, const char *b)
{ while (*a && *a == *b) { ++a; ++b; }
  return (unsigned char)*a - (unsigned char)*b; }

int strncmp(const char *a, const char *b, size_t n)
{ while (n-- && *a && *a == *b) { ++a; ++b; }
  if (n == (size_t)-1) return 0;
  return (unsigned char)*a - (unsigned char)*b; }

char *strcpy(char *d, const char *s)
{ char *p = d; while ((*p++ = *s++)) {} return d; }

char *strncpy(char *d, const char *s, size_t n)
{ size_t i = 0; while (i < n && s[i]) { d[i] = s[i]; ++i; }
  while (i < n) d[i++] = '\0'; return d; }

char *strcat(char *d, const char *s)
{ char *p = d; while (*p) ++p; while ((*p++ = *s++)) {} return d; }

char *strchr(const char *s, int c)
{ while (*s) { if (*s == (char)c) return (char *)s; ++s; }
  return c == 0 ? (char *)s : 0; }

char *strrchr(const char *s, int c)
{ const char *p = 0; do { if (*s == (char)c) p = s; } while (*s++);
  return (char *)p; }

char *strstr(const char *h, const char *n)
{
    if (!*n) return (char *)h;
    for (; *h; ++h) {
        const char *a = h, *b = n;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (!*b) return (char *)h;
    }
    return 0;
}

char *strdup(const char *s)
{
    extern void *malloc(size_t);
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (!p) return 0;
    for (size_t i = 0; i < n; ++i) p[i] = s[i];
    return p;
}
/*===OmniBridgeOs/usr/lib/oblibc/string.c 结束===*/