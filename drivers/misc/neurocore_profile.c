// SPDX-License-Identifier: GPL-2.0
/*
 * NeuroCore kernel profiles.
 *
 * Single knob: /sys/kernel/neurocore/profile
 *   balanced (default) | performance | powersave
 *
 * Applies one consistent tune across cpu-boost (touch floor), the
 * Adreno idler (GPU downscaling) and the kernelspace battery saver.
 * FKM/profile apps can switch at runtime; switching back restores
 * every knob, nothing stays stuck from a previous profile.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/battery_saver.h>

#include "../devfreq/adreno_idler.h"

#ifdef CONFIG_CPU_BOOST
extern void cpuboost_set_input_boost_ms(unsigned int ms);
extern void cpuboost_set_sched_boost_on_input(unsigned int v);
extern void cpuboost_set_boost_freq(unsigned int little_khz,
				    unsigned int big_khz);
#endif

enum neuro_profile {
	NEURO_BALANCED = 0,
	NEURO_PERFORMANCE,
	NEURO_POWERSAVE,
	NEURO_NPROFILES,
};

static const char *const neuro_profile_names[NEURO_NPROFILES] = {
	[NEURO_BALANCED] = "balanced",
	[NEURO_PERFORMANCE] = "performance",
	[NEURO_POWERSAVE] = "powersave",
};

static int neuro_current_profile = NEURO_BALANCED;
static struct kobject *neuro_kobj;

static void neuro_apply_profile(int p)
{
	switch (p) {
	case NEURO_PERFORMANCE:
#ifdef CONFIG_CPU_BOOST
		cpuboost_set_input_boost_ms(150);
		cpuboost_set_boost_freq(1804800, 2016000);
		cpuboost_set_sched_boost_on_input(1);
#endif
#ifdef CONFIG_ADRENO_IDLER
		adreno_idler_set_active(0);
#endif
		update_battery_saver(false);
		break;
	case NEURO_POWERSAVE:
#ifdef CONFIG_CPU_BOOST
		cpuboost_set_input_boost_ms(40);
		cpuboost_set_boost_freq(0, 0);
		cpuboost_set_sched_boost_on_input(0);
#endif
#ifdef CONFIG_ADRENO_IDLER
		adreno_idler_set_active(1);
#endif
		update_battery_saver(true);
		break;
	case NEURO_BALANCED:
	default:
#ifdef CONFIG_CPU_BOOST
		cpuboost_set_input_boost_ms(40);
		cpuboost_set_boost_freq(1363200, 1401600);
		cpuboost_set_sched_boost_on_input(0);
#endif
#ifdef CONFIG_ADRENO_IDLER
		adreno_idler_set_active(1);
#endif
		update_battery_saver(false);
		p = NEURO_BALANCED;
		break;
	}
	neuro_current_profile = p;
	pr_info("neurocore: profile -> %s\n", neuro_profile_names[p]);
}

static ssize_t profile_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n",
			neuro_profile_names[neuro_current_profile]);
}

static ssize_t profile_store(struct kobject *kobj,
			     struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	int i;

	for (i = 0; i < NEURO_NPROFILES; i++) {
		if (sysfs_streq(buf, neuro_profile_names[i])) {
			neuro_apply_profile(i);
			return count;
		}
	}
	if (!strcmp(buf, "0\n")) {
		neuro_apply_profile(NEURO_BALANCED);
		return count;
	}
	if (!strcmp(buf, "1\n")) {
		neuro_apply_profile(NEURO_PERFORMANCE);
		return count;
	}
	if (!strcmp(buf, "2\n")) {
		neuro_apply_profile(NEURO_POWERSAVE);
		return count;
	}
	return -EINVAL;
}

static ssize_t available_profiles_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "balanced performance powersave\n");
}

static struct kobj_attribute profile_attr =
	__ATTR(profile, 0664, profile_show, profile_store);
static struct kobj_attribute available_profiles_attr =
	__ATTR(available_profiles, 0444, available_profiles_show, NULL);

/*
 * Biofrost/FKM-compatible alias: /sys/kernel/kprofiles/kp_mode
 * Accepts the same names plus 0/1/2 (0=balanced, 1=performance,
 * 2=powersave) so FKM Custom Tunables and Biofrost-style scripts
 * work unchanged. Shows the current profile name.
 */
static ssize_t kp_mode_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	return profile_show(kobj, attr, buf);
}

static ssize_t kp_mode_store(struct kobject *kobj,
			     struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	if (!strcmp(buf, "0\n"))
		return profile_store(kobj, attr, "balanced", 9);
	if (!strcmp(buf, "1\n"))
		return profile_store(kobj, attr, "performance", 12);
	if (!strcmp(buf, "2\n"))
		return profile_store(kobj, attr, "powersave", 10);
	return profile_store(kobj, attr, buf, count);
}

static struct kobj_attribute kp_mode_attr =
	__ATTR(kp_mode, 0664, kp_mode_show, kp_mode_store);
static struct kobject *kprofiles_kobj;

static int __init neurocore_profile_init(void)
{
	int ret;

	neuro_kobj = kobject_create_and_add("neurocore", kernel_kobj);
	if (!neuro_kobj)
		return -ENOMEM;
	ret = sysfs_create_file(neuro_kobj, &profile_attr.attr);
	if (ret)
		goto err;
	ret = sysfs_create_file(neuro_kobj, &available_profiles_attr.attr);
	if (ret)
		goto err_profile;
	kprofiles_kobj = kobject_create_and_add("kprofiles", kernel_kobj);
	if (!kprofiles_kobj) {
		ret = -ENOMEM;
		goto err_available;
	}
	ret = sysfs_create_file(kprofiles_kobj, &kp_mode_attr.attr);
	if (ret)
		goto err_kprofiles;
	/* Defaults everywhere already equal balanced; normalize anyway. */
	neuro_apply_profile(NEURO_BALANCED);
	return 0;

err_kprofiles:
	kobject_put(kprofiles_kobj);
err_available:
	sysfs_remove_file(neuro_kobj, &available_profiles_attr.attr);
err_profile:
	sysfs_remove_file(neuro_kobj, &profile_attr.attr);
err:
	kobject_put(neuro_kobj);
	return ret;
}
late_initcall(neurocore_profile_init);
