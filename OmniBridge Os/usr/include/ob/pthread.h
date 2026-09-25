/*===OmniBridgeOs/usr/include/ob/pthread.h===*/
#ifndef OB_USER_PTHREAD_H
#define OB_USER_PTHREAD_H

#include <stdint.h>
#include <stddef.h>

typedef unsigned long pthread_t;

typedef struct {
    volatile int     lock;
    volatile int     waiters;
    uint32_t         _pad;
} pthread_mutex_t;

typedef struct {
    volatile int     seq;
    uint32_t         _pad;
    void            *mutex;
} pthread_cond_t;

typedef struct {
    uint32_t  attr;
    uint32_t  _pad;
} pthread_attr_t;

#define PTHREAD_MUTEX_INITIALIZER   { 0, 0, 0 }
#define PTHREAD_COND_INITIALIZER    { 0, 0, 0 }

int pthread_create(pthread_t *th, const pthread_attr_t *attr,
                   void *(*fn)(void *), void *arg);
int pthread_join(pthread_t th, void **retval);
void pthread_exit(void *retval) __attribute__((noreturn));
pthread_t pthread_self(void);

int pthread_mutex_init(pthread_mutex_t *m, const void *attr);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_mutex_destroy(pthread_mutex_t *m);

int pthread_cond_init(pthread_cond_t *c, const void *attr);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
int pthread_cond_destroy(pthread_cond_t *c);

/* TLS */
#define PTHREAD_KEYS_MAX 32
int   pthread_key_create(unsigned *key, void (*dtor)(void *));
int   pthread_setspecific(unsigned key, const void *value);
void *pthread_getspecific(unsigned key);

#endif /* OB_USER_PTHREAD_H */
/*===OmniBridgeOs/usr/include/ob/pthread.h 结束===*/