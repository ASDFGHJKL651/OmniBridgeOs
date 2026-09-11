#ifndef OMNIBRIDGE_PRINTK_H
#define OMNIBRIDGE_PRINTK_H

void printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define KERN_INFO "[INFO] "
#define KERN_WARN "[WARN] "
#define KERN_ERR  "[ERR ] "

#endif