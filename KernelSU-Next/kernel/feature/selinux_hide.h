#ifndef __KSU_H_SELINUX_HIDE
#define __KSU_H_SELINUX_HIDE

#include <linux/version.h>

struct policydb;

void ksu_selinux_hide_init();
void ksu_selinux_hide_exit();
void ksu_selinux_hide_drop_backup_if_unused();
void ksu_selinux_hide_handle_second_stage();
void ksu_selinux_hide_handle_post_fs_data();
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0)
// 4.14 only: snapshot live policy before first KSU rule insertion.
// Currently a no-op stub (full 4.14 hide needs 5.x AVC/policydb APIs).
// Declared here, defined in selinux_hide.c so rules.c links on 4.14.
void ksu_selinux_backup_for_hide_414(struct policydb *live);
#else
// No-op on 5.x (backup lives in rules.c there).
static inline void ksu_selinux_backup_for_hide_414(struct policydb *live)
{
	(void)live;
}
#endif

#endif
