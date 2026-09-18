#ifndef __KSU_H_MANUAL_HOOK
#define __KSU_H_MANUAL_HOOK

#include <linux/types.h>
#include <linux/fs.h>

struct filename;

/* manual execve pre-hook: ksud handling + su->ksud redirect.
 * kern_p may be replaced (old put, new from ksud path). */
void ksu_manual_execve(struct filename **kern_p,
		       const char __user *const __user *argv);
/* manual faccessat pre-hook: su->ksud path swap.
 * Cred-aware: evaluates access with ksu_cred and returns 0 when access is
 * granted (caller must return 0 immediately), 1 to proceed with the
 * original lookup. Needed because the swap alone still resolves with
 * caller creds, which fails once /data/adb is locked to 0700. */
int ksu_manual_faccessat(const char __user **filename_user, int mode);
void ksu_manual_stat(const char __user **filename_user);
/* manual read pre-hook: init.rc proxy install. */
void ksu_manual_read_hook(unsigned int fd);
/* manual reboot pre-hook. */
int ksu_manual_reboot(int magic1, int magic2, unsigned int cmd,
		       void __user *arg);
/* manual setresuid post-hook. */
void ksu_manual_setresuid(uid_t old_uid, uid_t new_uid);

#endif
