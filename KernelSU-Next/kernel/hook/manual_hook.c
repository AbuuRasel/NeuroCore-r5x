#include <linux/cred.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/namei.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>
#include <asm/current.h>
#include <linux/ptrace.h>

#include "hook/manual_hook.h"
#include "hook/setuid_hook.h"
#include "klog.h" // IWYU pragma: keep
#include "ksu.h"
#include "policy/allowlist.h"
#include "policy/app_profile.h"
#include "runtime/ksud.h"

extern void ksu_handle_execveat_ksud(const char *path, void *argv);

struct ksu_manual_arg_ptr {
#ifdef CONFIG_COMPAT
	bool is_compat;
#endif
	union {
		const char __user *const __user *native;
#ifdef CONFIG_COMPAT
		const compat_uptr_t __user *compat;
#endif
	} ptr;
};

#define SU_PATH "/system/bin/su"

static char __user *ksu_manual_stack_path(const char *kpath)
{
	size_t len = strlen(kpath) + 1;
	unsigned long sp = current_user_stack_pointer();
	char __user *p;

	sp = (sp - len - 256) & ~0xFUL;
	p = (char __user *)sp;
	return copy_to_user(p, kpath, len) ? NULL : p;
}

void ksu_manual_execve(struct filename **kern_p,
		       const char __user *const __user *argv)
{
	struct filename *kern = kern_p ? *kern_p : NULL;
	char kpath[32];
	struct ksu_manual_arg_ptr argv_u;
	if (!kern || IS_ERR(kern))
		return;

	memset(kpath, 0, sizeof(kpath));
	strncpy(kpath, kern->name, sizeof(kpath) - 1);

	memset(&argv_u, 0, sizeof(argv_u));
	argv_u.ptr.native = argv;
	ksu_handle_execveat_ksud(kpath, &argv_u);

	if (ksu_is_allow_uid_for_current(current_uid().val) &&
	    !memcmp(kern->name, SU_PATH, sizeof(SU_PATH))) {
		char __user *ksud_path = ksu_manual_stack_path(KSUD_PATH);
		struct filename *swapped;

		if (!ksud_path)
			return;
		swapped = getname(ksud_path);
		if (IS_ERR(swapped))
			return;
		putname(kern);
		*kern_p = swapped;
		pr_info("manual execve su->ksud\n");
		if (escape_with_root_profile())
			pr_err("manual execve escape_with_root_profile failed\n");
	}
}

static bool ksu_manual_is_su_path(const char __user **filename_user)
{
	char path[sizeof(SU_PATH) + 1];

	if (!filename_user || !*filename_user)
		return false;
	memset(path, 0, sizeof(path));
	if (strncpy_from_user(path, *filename_user, sizeof(path)) < 0)
		return false;
	return !memcmp(path, SU_PATH, sizeof(SU_PATH));
}

void ksu_manual_faccessat(const char __user **filename_user)
{
	char __user *ksud_path;

	if (!ksu_is_allow_uid_for_current(current_uid().val))
		return;
	if (!ksu_manual_is_su_path(filename_user))
		return;
	ksud_path = ksu_manual_stack_path(KSUD_PATH);
	if (!ksud_path)
		return;
	*filename_user = ksud_path;
	pr_info("manual faccessat su->ksud\n");
}

void ksu_manual_stat(const char __user **filename_user)
{
	ksu_manual_faccessat(filename_user);
}

void ksu_manual_setresuid(uid_t old_uid, uid_t new_uid)
{
	ksu_handle_setresuid(old_uid, new_uid);
}
