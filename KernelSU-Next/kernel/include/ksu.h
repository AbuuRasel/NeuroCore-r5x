#ifndef __KSU_H_KSU

/* 4.14 compat: strncpy_from_user_nofault does not exist below 5.x.
 * Manual-hook and syscall contexts here are sleepable, plain copy is fine. */
#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0)
#ifndef strncpy_from_user_nofault
#define strncpy_from_user_nofault(dst, src, count) strncpy_from_user(dst, src, count)
#endif
#endif
#define __KSU_H_KSU

#include <linux/types.h>
#include <linux/cred.h>
#include <linux/workqueue.h>

#define KERNEL_SU_VERSION KSU_VERSION
#define KERNEL_SU_VERSION_TAG KSU_VERSION_TAG

extern struct cred *ksu_cred;
extern bool ksu_late_loaded;
extern bool allow_shell;
extern struct selinux_policy *backup_sepolicy;
extern bool ksu_no_custom_rc;

static inline int startswith(char *s, char *prefix)
{
	return strncmp(s, prefix, strlen(prefix));
}

static inline int endswith(const char *s, const char *t)
{
	size_t slen = strlen(s);
	size_t tlen = strlen(t);
	if (tlen > slen)
		return 1;
	return strcmp(s + slen - tlen, t);
}

/* 4.14 has no untagged_addr; identity is safe here. */
#ifndef untagged_addr
#define untagged_addr(addr)	((addr) & ~(0xffUL << 56))
#endif
#ifndef copy_from_user_nofault
#define copy_from_user_nofault(dst, src, size) copy_from_user(dst, src, size)
#endif
#ifndef copy_to_user_nofault
#define copy_to_user_nofault(dst, src, size) copy_to_user(dst, src, size)
#endif
/* 4.14: boottime via legacy getboottime (timespec layout matches) */
#include <linux/time.h>
#ifndef ktime_get_boottime_ts64
#define ktime_get_boottime_ts64(ts) getboottime(ts)
#endif
/* 4.14 task_work_add takes a bool notify */
#ifndef TWA_RESUME
#define TWA_RESUME true
#endif
#ifndef fallthrough
#define fallthrough do {} while (0)
#endif
#endif
