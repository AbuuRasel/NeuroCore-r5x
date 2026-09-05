#ifndef __KSU_H_UTIL
#define __KSU_H_UTIL

#include "linux/fdtable.h" // IWYU pragma: keep
#include <linux/version.h>
#include <linux/syscalls.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
#define ksu_close_fd close_fd
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
#define ksu_close_fd ksys_close
#else
/* 4.14: no ksys_close, sys_close has the same prototype */
#define ksu_close_fd sys_close
#endif

#endif
