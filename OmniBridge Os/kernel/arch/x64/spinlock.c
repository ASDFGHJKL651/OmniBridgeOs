/*
 * spinlock.c —— 预留实现文件。
 *
 * 本步所有自旋锁接口均实现为 spinlock.h 中的 static inline 函数，
 * 没有需要在此文件中单独编译的符号。此文件保留以保持目录结构与
 * Makefile 的自动发现逻辑一致（Makefile 用 find 收集所有 .c 文件）。
 *
 * 后续若需要引入 NUMA 感知锁、读写自旋锁、MCS 队列锁等，可将其实
 * 现放在本文件并在 spinlock.h 中声明。
 */

#include "spinlock.h"

/* 空编译单元：避免部分工具链对空 .c 的警告 */
typedef int ob_spinlock_translation_unit_marker;