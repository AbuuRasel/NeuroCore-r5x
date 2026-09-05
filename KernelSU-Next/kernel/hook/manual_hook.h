#ifndef __KSU_H_MANUAL_HOOK
#define __KSU_H_MANUAL_HOOK

#include <linux/types.h>
#include <linux/fs.h>

struct filename;

/* manual execve pre-hook: ksud handling + su->ksud redirect.
 * kern_p may be replaced (old put, new from ksud path). */
void ksu_manual_execve(struct filename **kern_p,
		       const char __user *const __user *argv);
/* manual faccessat/stat pre-hooks: su->ksud path swap. */
void ksu_manual_faccessat(const char __user **filename_user);
void ksu_manual_stat(const char __user **filename_user);
/* manual read pre-hook: init.rc proxy install. */
void ksu_manual_read_hook(unsigned int fd);
/* manual reboot pre-hook. */
int ksu_manual_reboot(int magic1, int magic2, unsigned int cmd,
		       void __user *arg);
/* manual setresuid post-hook. */
void ksu_manual_setresuid(uid_t old_uid, uid_t new_uid);

#endif
