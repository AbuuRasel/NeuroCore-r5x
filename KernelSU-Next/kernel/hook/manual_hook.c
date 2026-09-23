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

/* NeuroCore dmesg-oracle hygiene: info logs exist only in DEBUG builds.
 * Empty body (not no_printk): format strings never reach the compiler,
 * so scanners cannot fingerprint them. pr_err stays for real failures. */
#ifndef CONFIG_KSU_DEBUG
#undef pr_info
#define pr_info(...) do { } while (0)
#endif

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
/* NeuroCore: same extra entry points as the syscall-table sucompat hook,
 * so direct su invocations keep working with /data/adb at 0700. */
#define KSU_SU_BIN_PATH "/data/adb/ksu/bin/su"

static char __user *ksu_manual_stack_path(const char *kpath)
{
	size_t len = strlen(kpath) + 1;
	unsigned long sp = current_user_stack_pointer();
	char __user *p;

	sp = (sp - len - 256) & ~0xFUL;
	p = (char __user *)sp;
	return copy_to_user(p, kpath, len) ? NULL : p;
}

/* Kernel-space exact match against the su entry points (NUL-inclusive,
 * length-guarded so overlong names can never false-match). */
static bool ksu_manual_is_su_kpath(const char *kpath, size_t len)
{
	if (len >= sizeof(SU_PATH) &&
	    !memcmp(kpath, SU_PATH, sizeof(SU_PATH)))
		return true;
	if (len >= sizeof(KSU_SU_BIN_PATH) &&
	    !memcmp(kpath, KSU_SU_BIN_PATH, sizeof(KSU_SU_BIN_PATH)))
		return true;
	if (len >= sizeof(KSUD_PATH) &&
	    !memcmp(kpath, KSUD_PATH, sizeof(KSUD_PATH)))
		return true;
	return false;
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
	    ksu_manual_is_su_kpath(kern->name, sizeof(kpath))) {
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
	char path[32];

	if (!filename_user || !*filename_user)
		return false;
	memset(path, 0, sizeof(path));
	if (strncpy_from_user(path, *filename_user, sizeof(path)) < 0)
		return false;
	return ksu_manual_is_su_kpath(path, sizeof(path));
}

int ksu_manual_faccessat(const char __user **filename_user, int mode)
{
	struct path p;
	struct inode *inode;
	const struct cred *old_cred;
	int err;

	if (!ksu_is_allow_uid_for_current(current_uid().val))
		return 1;
	if (!ksu_manual_is_su_path(filename_user))
		return 1;
	if (!ksu_cred)
		return 1;
	/* Evaluate access with ksu_cred (privileged SELinux domain, root DAC):
	 * the swapped path below would otherwise resolve with caller creds
	 * and fail once /data/adb is locked to 0700. faccessat mode bits
	 * (R/W/X_OK) numerically equal MAY_* values, so pass through. */
	old_cred = override_creds(ksu_cred);
	err = kern_path(KSUD_PATH, LOOKUP_FOLLOW, &p);
	if (!err) {
		inode = d_backing_inode(p.dentry);
		err = inode ? inode_permission(inode,
				mode & (MAY_READ | MAY_WRITE | MAY_EXEC)) :
			      -ENOENT;
		path_put(&p);
	} else {
		err = 1; /* ksud missing: let orig report ENOENT truthfully */
	}
	revert_creds(old_cred);
	if (!err) {
		char __user *ksud_path = ksu_manual_stack_path(KSUD_PATH);
		if (ksud_path)
			*filename_user = ksud_path;
		pr_info("manual faccessat su->ksud (0700-safe)\n");
		return 0;
	}
	return 1;
}

void ksu_manual_stat(const char __user **filename_user)
{
	char __user *ksud_path;

	/* Deliberately swap-only (no cred override): stat stays strict so
	 * detectors probing su paths learn nothing; tsu-style existence
	 * checks use faccessat, which is handled cred-safely above. */
	if (!ksu_is_allow_uid_for_current(current_uid().val))
		return;
	if (!ksu_manual_is_su_path(filename_user))
		return;
	ksud_path = ksu_manual_stack_path(KSUD_PATH);
	if (!ksud_path)
		return;
	*filename_user = ksud_path;
	pr_info("manual stat su->ksud\n");
}

void ksu_manual_setresuid(uid_t old_uid, uid_t new_uid)
{
	ksu_handle_setresuid(old_uid, new_uid);
}
