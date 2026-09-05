#include <linux/cred.h>
#include <linux/uidgid.h>
#include <linux/susfs.h>
#include <linux/susfs_def.h>

#include "klog.h" // IWYU pragma: keep

int ksu_handle_susfs_reboot(unsigned int cmd, void __user **arg)
{
	if (current_uid().val != 0)
		return 0;

	switch (cmd) {
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	case CMD_SUSFS_ADD_SUS_PATH:
		susfs_add_sus_path(arg);
		return 0;
	case CMD_SUSFS_ADD_SUS_PATH_LOOP:
		susfs_add_sus_path_loop(arg);
		return 0;
#endif
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
	case CMD_SUSFS_HIDE_SUS_MNTS_FOR_NON_SU_PROCS:
		susfs_set_hide_sus_mnts_for_non_su_procs(arg);
		return 0;
#endif
#ifdef CONFIG_KSU_SUSFS_SUS_KSTAT
	case CMD_SUSFS_ADD_SUS_KSTAT:
	case CMD_SUSFS_ADD_SUS_KSTAT_STATICALLY:
		susfs_add_sus_kstat(arg);
		return 0;
	case CMD_SUSFS_UPDATE_SUS_KSTAT:
		susfs_update_sus_kstat(arg);
		return 0;
#endif
#ifdef CONFIG_KSU_SUSFS_SUS_MAP
	case CMD_SUSFS_ADD_SUS_MAP:
		susfs_add_sus_map(arg);
		return 0;
#endif
#ifdef CONFIG_KSU_SUSFS_SUS_MEMFD
	case CMD_SUSFS_ADD_SUS_MEMFD:
		susfs_add_sus_memfd(arg);
		return 0;
#endif
#ifdef CONFIG_KSU_SUSFS_SPOOF_UNAME
	case CMD_SUSFS_SET_UNAME:
		susfs_set_uname(arg);
		return 0;
#endif
#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
	case CMD_SUSFS_ENABLE_LOG:
		susfs_enable_log(arg);
		return 0;
#endif
#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
	case CMD_SUSFS_SET_CMDLINE_OR_BOOTCONFIG:
		susfs_set_cmdline_or_bootconfig(arg);
		return 0;
#endif
#ifdef CONFIG_KSU_SUSFS_OPEN_REDIRECT
	case CMD_SUSFS_ADD_OPEN_REDIRECT:
		susfs_add_open_redirect(arg);
		return 0;
#endif
	case CMD_SUSFS_ENABLE_AVC_LOG_SPOOFING:
		susfs_set_avc_log_spoofing(arg);
		return 0;
	case CMD_SUSFS_SHOW_ENABLED_FEATURES:
		susfs_get_enabled_features(arg);
		return 0;
	case CMD_SUSFS_SHOW_VARIANT:
		susfs_show_variant(arg);
		return 0;
	case CMD_SUSFS_SHOW_VERSION:
		susfs_show_version(arg);
		return 0;
	default:
		return -EINVAL;
	}
}
