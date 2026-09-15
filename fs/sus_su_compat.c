/* NeuroCore glue: provide the two symbols fs/sus_su.c expects,
 * mapped to KernelSU-Next equivalents.
 * - susfs_is_allow_su() mirrors upstream susfs4ksu logic
 *   (manager always allowed, else allowlist check).
 * - ksu_escape_to_root() maps to escape_with_root_profile().
 */
#include <linux/cred.h>
#include <linux/uidgid.h>
#include <linux/compiler.h>

extern bool __ksu_is_allow_uid(uid_t uid);
extern int escape_with_root_profile(void);
extern uid_t ksu_manager_appid;

#define KSU_PER_USER_RANGE 100000

bool susfs_is_allow_su(void)
{
#ifdef CONFIG_KSU_DISABLE_MANAGER
	if (current_uid().val == 0)
		return true;
#else
	if (unlikely(ksu_manager_appid == current_uid().val % KSU_PER_USER_RANGE))
		return true;
#endif
	return unlikely(__ksu_is_allow_uid(current_uid().val));
}

void ksu_escape_to_root(void)
{
	escape_with_root_profile();
}
