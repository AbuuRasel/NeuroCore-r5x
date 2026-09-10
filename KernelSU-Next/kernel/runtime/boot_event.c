#include "feature/selinux_hide.h"
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/printk.h>

#include "policy/allowlist.h"
#include "klog.h" // IWYU pragma: keep
#include "runtime/ksud_boot.h"
#include "runtime/ksud.h"
#include "manager/manager_observer.h"
#include "manager/throne_tracker.h"
#include "supercall/internal.h"
#include "selinux/sepolicy.h"
#include "selinux/selinux.h"
#include "ss/services.h"

/* TEMP mirror: shared in-memory narrative (see rules.c). */
#ifndef KSU_BOOTLOG_DECL
#define KSU_BOOTLOG_DECL
extern void ksu_bootlog(const char *msg);
#endif
static void boot_mark(const char *msg)
{
	ksu_bootlog(msg);
}

bool ksu_module_mounted __read_mostly = false;
bool ksu_boot_completed __read_mostly = false;

extern void ksu_avc_spoof_late_init(void);

/* TEMP narrative helper is in rules.c; mirrored tiny logger here. */
extern void ksu_bootlog(const char *msg);

/* NeuroCore: guarantee base rules exist at every post-load checkpoint.
 * Custom ROMs may load/reload policy after second_stage, wiping the
 * boot-time application; post-fs-data and boot-completed both run long
 * after the policy is live, so re-apply if ksu is missing. */
static void ksu_ensure_rules(const char *where)
{
    char msg[96];

    if (ksu_exists(&policydb, KERNEL_SU_DOMAIN)) {
        scnprintf(msg, sizeof(msg), "%s: ksu PRESENT, no heal needed",
                  where);
        ksu_bootlog(msg);
        return;
    }
    pr_warn("%s: ksu rules missing, re-applying\n", where);
    scnprintf(msg, sizeof(msg), "%s: ksu MISSING, re-applying", where);
    ksu_bootlog(msg);
    apply_kernelsu_rules();
    ksu_load_allow_list();
    boot_mark(ksu_exists(&policydb, KERNEL_SU_DOMAIN) ?
              "heal OK, ksu PRESENT" : "heal FAILED, ksu MISSING");
}

void on_post_fs_data(void)
{
    static bool done = false;

    if (done) {
        pr_info("on_post_fs_data already done\n");
        return;
    }

    done = true;
    pr_info("on_post_fs_data!\n");

    ksu_load_allow_list();
    ksu_ensure_rules("post-fs-data");
    ksu_observer_init();
    // Sanity check for safe mode only needs early-boot input samples.
    ksu_stop_input_hook_runtime();
    ksu_selinux_hide_handle_post_fs_data();
}

extern void ext4_unregister_sysfs(struct super_block *sb);

int nuke_ext4_sysfs(const char *mnt)
{
    struct path path;
    int err = kern_path(mnt, 0, &path);

    if (err) {
        pr_err("nuke path err: %d\n", err);
        return err;
    }

    if (strcmp(path.dentry->d_inode->i_sb->s_type->name, "ext4") != 0) {
        pr_info("nuke but module aren't mounted\n");
        path_put(&path);
        return -EINVAL;
    }

    ext4_unregister_sysfs(path.dentry->d_inode->i_sb);
    path_put(&path);
    return 0;
}

void on_module_mounted(void)
{
    pr_info("on_module_mounted!\n");
    ksu_module_mounted = true;
}

void on_boot_completed(void)
{
    ksu_boot_completed = true;
    pr_info("on_boot_completed!\n");
    track_throne(true);
    /* NeuroCore self-heal: if the boot-time KSU rules never landed
     * (missed second_stage or a dropped stop_machine op), the live
     * policy has no ksu domain and su can never be granted. Re-apply
     * now while locking is safe; harmless if already applied. */
    ksu_ensure_rules("boot_completed");
    ksu_selinux_hide_drop_backup_if_unused();
    ksu_avc_spoof_late_init();
}
