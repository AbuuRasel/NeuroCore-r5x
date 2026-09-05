// SPDX-License-Identifier: GPL-2.0-only
/*
 * NeuroCore sound_control - Bolero TX/RX digital gains via ALSA kcontrols.
 *
 * Sysfs interface is in dB (-84..+40). The codec SX controls use raw
 * 0..124 (raw = dB + 84); conversion happens here, so tinymix values
 * (raw) and sysfs values (dB) stay consistent:
 *   dB = raw - 84  (e.g. raw 84 = 0dB, raw 124 = +40dB)
 *
 * mic_gain:     TX_DEC0 + TX_DEC1 Volume (in-call mics)
 * speaker_gain: RX_RX0 + RX_RX1 Digital Volume (speaker path)
 * Boot default: mic = stock + 6dB (fixes low in-call mic), speaker = stock.
 * Stock mic values readable via mic_gain_stock (always reversible).
 * v1.3: mic and speaker pairs resolve independently so a missing/renamed
 * speaker control can no longer make mic_gain_stock return ENODEV, and a
 * cached stock value stays readable even if the codec is momentarily busy.
 * Use the read-only "controls" node to verify actual card/control names.
 * v1.4: stock snapshot (read) and +6dB boost (write) are split. A failed
 * boost write (e.g. TX macro down while idle) is logged, retried later,
 * and never blocks mic reads/writes. New "status" node exposes
 * mic_ok/spk_ok/stock_done/boost_done/boost_err for diagnosis.
 * v1.5: writes go straight to the codec register with the same bit math
 * as stock put_volsw_sx, because stock 4.14 put rejects any raw value
 * above max (40) - the whole normal range incl. the 0dB default (raw 84).
 * Range is validated against the real info range (0..124) instead.
 * v1.6: normalize update_bits change-flag (1) to success; only < 0 is
 * an error, otherwise manual stores falsely report failure and the auto
 * boost never marks itself done.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/bitops.h>
#include <sound/core.h>
#include <sound/control.h>
#include <sound/soc.h>

#define SC_DB_MIN	-84
#define SC_DB_MAX	40
#define SC_MIC_BOOST	6

static inline int sc_db_to_raw(int db)
{
	return db - SC_DB_MIN;
}

static inline int sc_raw_to_db(int raw)
{
	return raw + SC_DB_MIN;
}

static struct kobject *sc_kobj;
static DEFINE_MUTEX(sc_lock);

static struct snd_kcontrol *sc_mic[2];
static struct snd_kcontrol *sc_spk[2];
static int sc_mic_stock_db[2];
static bool sc_mic_ok;
static bool sc_spk_ok;
static bool sc_default_done;
static bool sc_boost_done;
static int sc_boost_err;

static const char *sc_mic_names[2] = { "TX_DEC0 Volume", "TX_DEC1 Volume" };
static const char *sc_spk_names[2] = { "RX_RX0 Digital Volume",
				       "RX_RX1 Digital Volume" };

static struct snd_kcontrol *sc_find_kctl(const char *name)
{
	int i;

	for (i = 0; i < SNDRV_CARDS; i++) {
		struct snd_card *card = snd_cards[i];
		struct snd_kcontrol *kctl;
		struct snd_ctl_elem_id id;

		if (!card)
			continue;
		memset(&id, 0, sizeof(id));
		id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
		strlcpy(id.name, name, sizeof(id.name));
		kctl = snd_ctl_find_id(card, &id);
		if (kctl)
			return kctl;
	}
	return NULL;
}

/* Read current dB. No global side effects; caller invalidates its pair. */
static int sc_kctl_get_db(struct snd_kcontrol *kctl, int *db)
{
	struct snd_ctl_elem_value uctl;
	int ret;

	if (!kctl || !db)
		return -ENODEV;
	memset(&uctl, 0, sizeof(uctl));
	ret = kctl->get(kctl, &uctl);
	if (ret)
		return -EIO;
	*db = sc_raw_to_db((int)uctl.value.integer.value[0]);
	return 0;
}

/*
 * Write raw (0..levels) to a Bolero SX volume control.
 *
 * Stock 4.14 snd_soc_put_volsw_sx rejects any value > max (40), i.e. the
 * whole normal range including the 0dB default (raw 84) - proven on-device
 * (boost put failed even though info advertises 0..124 and reads work).
 * This replicates the stock register math bit-for-bit
 * (reg = (raw + min) & mask, same mask formula) but validates against the
 * real info range (levels = max - min) instead of max.
 * Returns 0 or the actual error code (no masking).
 */
static int sc_kctl_set_raw(struct snd_kcontrol *kctl, int raw)
{
	struct snd_soc_component *c;
	struct soc_mixer_control *mc;
	unsigned int mask, val, val_mask;
	int levels, ret;

	if (!kctl)
		return -ENODEV;
	mc = (struct soc_mixer_control *)kctl->private_value;
	if (!mc || mc->max <= mc->min)
		return -EINVAL;
	levels = mc->max - mc->min; /* 124, matches info max */
	if (raw < 0 || raw > levels)
		return -EINVAL;
	c = snd_soc_kcontrol_component(kctl);
	if (!c)
		return -ENODEV;
	mask = (1U << (fls(mc->min + mc->max) - 1)) - 1;
	val_mask = mask << mc->shift;
	val = ((unsigned int)(raw + mc->min) & mask) << mc->shift;
	ret = snd_soc_component_update_bits(c, mc->reg, val_mask, val);
	/* update_bits returns 1 when the register changed: still success. */
	return ret < 0 ? ret : 0;
}

/* Write dB. Returns 0 or the actual error code (no masking). */
static int sc_kctl_set_db(struct snd_kcontrol *kctl, int db)
{
	if (!kctl)
		return -ENODEV;
	if (db < SC_DB_MIN || db > SC_DB_MAX)
		return -EINVAL;
	return sc_kctl_set_raw(kctl, sc_db_to_raw(db));
}

static void sc_invalidate_mic(void)
{
	sc_mic[0] = NULL;
	sc_mic[1] = NULL;
	sc_mic_ok = false;
}

static void sc_invalidate_spk(void)
{
	sc_spk[0] = NULL;
	sc_spk[1] = NULL;
	sc_spk_ok = false;
}

/* Resolve one pair at a time. Caller holds sc_lock. */
static bool sc_resolve_mic(void)
{
	int i;

	if (sc_mic_ok && sc_mic[0] && sc_mic[1])
		return true;
	for (i = 0; i < 2; i++) {
		if (!sc_mic[i])
			sc_mic[i] = sc_find_kctl(sc_mic_names[i]);
	}
	if (!sc_mic[0] || !sc_mic[1])
		return false;
	sc_mic_ok = true;
	return true;
}

static bool sc_resolve_spk(void)
{
	int i;

	if (sc_spk_ok && sc_spk[0] && sc_spk[1])
		return true;
	for (i = 0; i < 2; i++) {
		if (!sc_spk[i])
			sc_spk[i] = sc_find_kctl(sc_spk_names[i]);
	}
	if (!sc_spk[0] || !sc_spk[1])
		return false;
	sc_spk_ok = true;
	return true;
}

/*
 * Snapshot stock mic once (read-only, harmless). Caller holds sc_lock.
 * A failed read invalidates the mic pair so the next access re-resolves;
 * reads/writes are never blocked by the boost step below.
 */
static void sc_maybe_snapshot_mic(void)
{
	int v0, v1, r0, r1;

	if (sc_default_done)
		return;
	if (!sc_mic_ok)
		return;
	r0 = sc_kctl_get_db(sc_mic[0], &v0);
	r1 = sc_kctl_get_db(sc_mic[1], &v1);
	if (r0 || r1) {
		pr_err_ratelimited("sound_control: mic snapshot read failed (%d/%d)\n",
				   r0, r1);
		sc_invalidate_mic();
		return;
	}
	/* Sanity: only trust plausible stock values. */
	if (v0 < SC_DB_MIN || v0 > SC_DB_MAX ||
	    v1 < SC_DB_MIN || v1 > SC_DB_MAX) {
		pr_err("sound_control: mic snapshot out of range %d/%d dB\n",
		       v0, v1);
		return;
	}
	sc_mic_stock_db[0] = v0;
	sc_mic_stock_db[1] = v1;
	sc_default_done = true;
	pr_info("sound_control: mic stock %d/%d dB\n", v0, v1);
}

/*
 * Best-effort +6dB boost. A failed write (e.g. TX macro clocked down
 * while idle) is logged and retried on the next access but NEVER
 * invalidates the mic pair or blocks reads/writes. Caller holds sc_lock.
 */
static void sc_maybe_boost_mic(void)
{
	int v0, v1, r0, r1;

	if (sc_boost_done || !sc_default_done || !sc_mic_ok)
		return;
	v0 = sc_mic_stock_db[0];
	v1 = sc_mic_stock_db[1];
	if (v0 > SC_DB_MAX - SC_MIC_BOOST)
		v0 = SC_DB_MAX;
	else
		v0 += SC_MIC_BOOST;
	if (v1 > SC_DB_MAX - SC_MIC_BOOST)
		v1 = SC_DB_MAX;
	else
		v1 += SC_MIC_BOOST;
	r0 = sc_kctl_set_db(sc_mic[0], v0);
	r1 = sc_kctl_set_db(sc_mic[1], v1);
	if (r0 || r1) {
		if (!sc_boost_err)
			pr_err("sound_control: mic boost write failed (%d/%d), will retry\n",
			       r0, r1);
		sc_boost_err = r0 ? r0 : r1;
		return;
	}
	if (sc_boost_err)
		pr_info("sound_control: mic boost write recovered\n");
	sc_boost_err = 0;
	sc_boost_done = true;
	pr_info("sound_control: mic boosted to %d/%d dB\n", v0, v1);
}

/* Set both channels; error if EITHER fails (no silent split-brain). */
static int sc_set_pair(struct snd_kcontrol **pair, int db)
{
	int i, ret = 0, r;

	for (i = 0; i < 2; i++) {
		r = sc_kctl_set_db(pair[i], db);
		if (r && !ret)
			ret = r;
	}
	return ret;
}

static ssize_t mic_gain_show(struct kobject *kobj, struct kobj_attribute *attr,
			     char *buf)
{
	int v0, v1, ret;

	mutex_lock(&sc_lock);
	if (!sc_resolve_mic()) {
		mutex_unlock(&sc_lock);
		return -ENODEV;
	}
	sc_maybe_snapshot_mic();
	sc_maybe_boost_mic();
	if (sc_kctl_get_db(sc_mic[0], &v0) ||
	    sc_kctl_get_db(sc_mic[1], &v1)) {
		pr_err_ratelimited("sound_control: mic read failed\n");
		sc_invalidate_mic();
		mutex_unlock(&sc_lock);
		return -EIO;
	}
	ret = scnprintf(buf, PAGE_SIZE, "%d %d\n", v0, v1);
	mutex_unlock(&sc_lock);
	return ret;
}

static ssize_t mic_gain_store(struct kobject *kobj, struct kobj_attribute *attr,
			      const char *buf, size_t count)
{
	long val;
	int ret;

	if (kstrtol(buf, 10, &val))
		return -EINVAL;
	if (val < SC_DB_MIN || val > SC_DB_MAX)
		return -EINVAL;
	mutex_lock(&sc_lock);
	if (!sc_resolve_mic()) {
		mutex_unlock(&sc_lock);
		return -ENODEV;
	}
	sc_maybe_snapshot_mic();
	ret = sc_set_pair(sc_mic, (int)val);
	if (ret) {
		pr_err_ratelimited("sound_control: mic write failed\n");
		sc_invalidate_mic();
	} else {
		/* Manual set overrides the auto boost. */
		sc_boost_done = true;
		sc_boost_err = 0;
	}
	mutex_unlock(&sc_lock);
	return ret ? ret : (ssize_t)count;
}

static ssize_t speaker_gain_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	int v0, v1, ret;

	mutex_lock(&sc_lock);
	if (!sc_resolve_spk()) {
		mutex_unlock(&sc_lock);
		return -ENODEV;
	}
	if (sc_kctl_get_db(sc_spk[0], &v0) ||
	    sc_kctl_get_db(sc_spk[1], &v1)) {
		sc_invalidate_spk();
		mutex_unlock(&sc_lock);
		return -EIO;
	}
	ret = scnprintf(buf, PAGE_SIZE, "%d %d\n", v0, v1);
	mutex_unlock(&sc_lock);
	return ret;
}

static ssize_t speaker_gain_store(struct kobject *kobj,
				  struct kobj_attribute *attr, const char *buf,
				  size_t count)
{
	long val;
	int ret;

	if (kstrtol(buf, 10, &val))
		return -EINVAL;
	if (val < SC_DB_MIN || val > SC_DB_MAX)
		return -EINVAL;
	mutex_lock(&sc_lock);
	if (!sc_resolve_spk()) {
		mutex_unlock(&sc_lock);
		return -ENODEV;
	}
	ret = sc_set_pair(sc_spk, (int)val);
	if (ret)
		sc_invalidate_spk();
	mutex_unlock(&sc_lock);
	return ret ? ret : (ssize_t)count;
}

static ssize_t mic_gain_stock_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	int ret;

	mutex_lock(&sc_lock);
	/* Cached stock stays readable even if codec is briefly unavailable. */
	if (sc_default_done) {
		ret = scnprintf(buf, PAGE_SIZE, "%d %d\n", sc_mic_stock_db[0],
				sc_mic_stock_db[1]);
		mutex_unlock(&sc_lock);
		return ret;
	}
	if (!sc_resolve_mic()) {
		mutex_unlock(&sc_lock);
		return -ENODEV;
	}
	sc_maybe_snapshot_mic();
	sc_maybe_boost_mic();
	if (!sc_default_done) {
		mutex_unlock(&sc_lock);
		return -ENODEV;
	}
	ret = scnprintf(buf, PAGE_SIZE, "%d %d\n", sc_mic_stock_db[0],
			sc_mic_stock_db[1]);
	mutex_unlock(&sc_lock);
	return ret;
}

static ssize_t version_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return scnprintf(buf, PAGE_SIZE,
			 "NeuroCore sound_control v1.6 (dB, range -84..40)\n");
}

/* Debug: dump every mixer control name on every card (capped). */
static ssize_t controls_show(struct kobject *kobj, struct kobj_attribute *attr,
			     char *buf)
{
	int i, len = 0;

	for (i = 0; i < SNDRV_CARDS; i++) {
		struct snd_card *card = snd_cards[i];
		struct snd_kcontrol *kctl;

		if (!card)
			continue;
		len += scnprintf(buf + len, PAGE_SIZE - len, "[card%d:%s]\n",
				 i, card->id);
		if (len >= PAGE_SIZE - 64)
			break;
		down_read(&card->controls_rwsem);
		list_for_each_entry(kctl, &card->controls, list) {
			len += scnprintf(buf + len, PAGE_SIZE - len, "  %s\n",
					 kctl->id.name);
			if (len >= PAGE_SIZE - 64)
				break;
		}
		up_read(&card->controls_rwsem);
		if (len >= PAGE_SIZE - 64)
			break;
	}
	if (!len)
		len = scnprintf(buf, PAGE_SIZE, "(no cards)\n");
	return len;
}

static struct kobj_attribute mic_gain_attr =
	__ATTR(mic_gain, 0644, mic_gain_show, mic_gain_store);
static struct kobj_attribute speaker_gain_attr =
	__ATTR(speaker_gain, 0644, speaker_gain_show, speaker_gain_store);
static struct kobj_attribute mic_gain_stock_attr =
	__ATTR(mic_gain_stock, 0444, mic_gain_stock_show, NULL);
/* Diagnosis: resolve state + stock + boost error without needing dmesg. */
static ssize_t status_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	int ret;

	mutex_lock(&sc_lock);
	ret = scnprintf(buf, PAGE_SIZE,
			"mic_ok=%d spk_ok=%d stock_done=%d boost_done=%d boost_err=%d stock=%d/%d\n",
			sc_mic_ok, sc_spk_ok, sc_default_done, sc_boost_done,
			sc_boost_err, sc_mic_stock_db[0], sc_mic_stock_db[1]);
	mutex_unlock(&sc_lock);
	return ret;
}

static struct kobj_attribute version_attr =
	__ATTR(version, 0444, version_show, NULL);
static struct kobj_attribute controls_attr =
	__ATTR(controls, 0444, controls_show, NULL);
static struct kobj_attribute status_attr =
	__ATTR(status, 0444, status_show, NULL);

static struct attribute *sc_attrs[] = {
	&mic_gain_attr.attr,
	&speaker_gain_attr.attr,
	&mic_gain_stock_attr.attr,
	&version_attr.attr,
	&controls_attr.attr,
	&status_attr.attr,
	NULL,
};

static struct attribute_group sc_attr_group = {
	.attrs = sc_attrs,
};

static int __init sound_control_init(void)
{
	int ret;

	sc_kobj = kobject_create_and_add("sound_control", kernel_kobj);
	if (!sc_kobj)
		return -ENOMEM;
	ret = sysfs_create_group(sc_kobj, &sc_attr_group);
	if (ret) {
		kobject_put(sc_kobj);
		return ret;
	}
	pr_info("sound_control: ready (/sys/kernel/sound_control)\n");

	/* Try early resolve; first sysfs access retries if audio is down. */
	mutex_lock(&sc_lock);
	if (sc_resolve_mic()) {
		sc_maybe_snapshot_mic();
		sc_maybe_boost_mic();
	}
	sc_resolve_spk();
	mutex_unlock(&sc_lock);
	return 0;
}
late_initcall(sound_control_init);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("NeuroCore");
MODULE_DESCRIPTION("Bolero TX/RX digital gain control (dB)");
