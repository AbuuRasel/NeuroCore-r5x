// SPDX-License-Identifier: GPL-2.0-only
/*
 * NeuroCore sound_control - Bolero TX/RX digital gains via ALSA kcontrols.
 *
 * Backend is in dB (-84..+40). The codec SX controls use raw
 * 0..124 (raw = dB + 84); conversion happens here, so tinymix values
 * (raw) and sysfs values (dB) stay consistent:
 *   dB = raw - 84  (e.g. raw 84 = 0dB, raw 124 = +40dB)
 *
 * Sysfs (single UI to avoid confusion):
 * /sys/class/misc/soundcontrol/{mic_boost,speaker_l_boost,
 * speaker_r_boost} (0..20, FKM layout, sticky display).
 * Diagnosis: /sys/kernel/sound_control/{status,version,controls,
 * mic_gain_stock,speaker_pa_gain}.
 * mic_gain:     TX_DEC0 + TX_DEC1 Volume (in-call mics)
 * speaker_gain: RX_RX2 Digital Volume (speaker lives on RX2 on this
 *               platform - Biofrost-proven on realme sm6125. RX_RX0/RX1
 *               feed headphone/earpiece; WSA_RX writes land in the
 *               register but are not audible on the speaker.)
 * speaker_pa_gain: EAR SPKR PA Gain enum (0..7, 0 = stock). This is the
 *               hardware PA loudness lever; the wsa-macro driver applies
 *               it at stream start (POST_PMU), so it survives HAL/DAPM
 *               rewrites unlike live digital writes. Takes effect on the
 *               next track play / pause-resume, not mid-stream.
 * Boot default: mic = stock + 6dB (fixes low in-call mic), speaker = stock + 4dB.
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
 * v2.0: speaker pair moved to WSA_RX0/WSA_RX1 Digital Volume (smart
 * amps drive the speaker; RX_RX0/RX1 writes return success but are not
 * audible). find path takes controls_rwsem like the controls dump.
 * v3.0: speaker moved to RX_RX2 Digital Volume (Biofrost-proven route
 * on realme sm6125; WSA writes also inaudible). Both pair slots point
 * at the single RX2 control - writes are idempotent.
 * v3.1: new speaker_pa_gain node driving the EAR SPKR PA enum (0..7)
 * through the driver's own put path. PA gain is applied by wsa-macro
 * at stream start, so it persists across HAL/DAPM rewrites. Fully
 * reversible: 0 restores exact stock behavior.
 * v4.0: FKM (franco) compat misc device at
 * /sys/class/misc/soundcontrol/{mic_boost,speaker_l_boost,
 * speaker_r_boost} (0..20, sticky display values like franco's own
 * driver). Writes drive the same backend; both UIs stay in sync.
 * v4.1: single UI - the pair nodes (mic_gain/speaker_gain) are gone,
 * FKM misc nodes are the only sliders. Boot auto boost now triggers
 * on FKM access.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/bitops.h>
#include <linux/miscdevice.h>
#include <sound/core.h>
#include <sound/control.h>
#include <sound/soc.h>

#define SC_DB_MIN	-84
#define SC_DB_MAX	40
#define SC_MIC_BOOST	6
#define SC_SPK_BOOST	4

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
static int sc_spk_stock_db[2];
static bool sc_mic_ok;
static bool sc_spk_ok;
static bool sc_default_done;
static bool sc_boost_done;
static int sc_boost_err;
static bool sc_spk_default_done;
static bool sc_spk_boost_done;
static int sc_spk_boost_err;

static const char *sc_mic_names[2] = { "TX_DEC0 Volume", "TX_DEC1 Volume" };
static const char *sc_spk_names[2] = { "RX_RX2 Digital Volume",
				       "RX_RX2 Digital Volume" };

/* EAR SPKR PA enum (G_DEFAULT/G_0..G_6_DB); applied by wsa-macro at
 * stream start, never live. 0 = stock. */
static const char *sc_spk_pa_name = "EAR SPKR PA Gain";
static struct snd_kcontrol *sc_spk_pa;
static bool sc_spk_pa_ok;

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
		down_read(&card->controls_rwsem);
		kctl = snd_ctl_find_id(card, &id);
		up_read(&card->controls_rwsem);
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
 * Best-effort +6dB mic boost. A failed write (e.g. TX macro clocked down
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

/*
 * Speaker auto boost (+4dB over stock). Same best-effort pattern as the
 * mic path: snapshot stock once, apply once, manual sysfs writes override.
 * Caller holds sc_lock.
 */
static void sc_maybe_snapshot_spk(void)
{
	int v0, v1, r0, r1;

	if (sc_spk_default_done)
		return;
	if (!sc_spk_ok)
		return;
	r0 = sc_kctl_get_db(sc_spk[0], &v0);
	r1 = sc_kctl_get_db(sc_spk[1], &v1);
	if (r0 || r1) {
		pr_err_ratelimited("sound_control: spk snapshot read failed (%d/%d)\n",
				   r0, r1);
		sc_invalidate_spk();
		return;
	}
	if (v0 < SC_DB_MIN || v0 > SC_DB_MAX ||
	    v1 < SC_DB_MIN || v1 > SC_DB_MAX) {
		pr_err("sound_control: spk snapshot out of range %d/%d dB\n",
		       v0, v1);
		return;
	}
	sc_spk_stock_db[0] = v0;
	sc_spk_stock_db[1] = v1;
	sc_spk_default_done = true;
	pr_info("sound_control: spk stock %d/%d dB\n", v0, v1);
}

static void sc_maybe_boost_spk(void)
{
	int v0, v1, r0, r1;

	if (sc_spk_boost_done || !sc_spk_default_done || !sc_spk_ok)
		return;
	v0 = sc_spk_stock_db[0];
	v1 = sc_spk_stock_db[1];
	if (v0 > SC_DB_MAX - SC_SPK_BOOST)
		v0 = SC_DB_MAX;
	else
		v0 += SC_SPK_BOOST;
	if (v1 > SC_DB_MAX - SC_SPK_BOOST)
		v1 = SC_DB_MAX;
	else
		v1 += SC_SPK_BOOST;
	r0 = sc_kctl_set_db(sc_spk[0], v0);
	r1 = sc_kctl_set_db(sc_spk[1], v1);
	if (r0 || r1) {
		if (!sc_spk_boost_err)
			pr_err("sound_control: spk boost write failed (%d/%d), will retry\n",
			       r0, r1);
		sc_spk_boost_err = r0 ? r0 : r1;
		return;
	}
	if (sc_spk_boost_err)
		pr_info("sound_control: spk boost write recovered\n");
	sc_spk_boost_err = 0;
	sc_spk_boost_done = true;
	pr_info("sound_control: spk boosted to %d/%d dB\n", v0, v1);
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

/* Locked mic/speaker setters shared by both sysfs frontends.
 * Caller holds sc_lock. Returns 0 or error code. */
static int sc_mic_set_db_locked(int db)
{
	int ret;

	if (!sc_resolve_mic())
		return -ENODEV;
	sc_maybe_snapshot_mic();
	ret = sc_set_pair(sc_mic, db);
	if (ret)
		sc_invalidate_mic();
	else {
		/* Manual set overrides the auto boost. */
		sc_boost_done = true;
		sc_boost_err = 0;
	}
	return ret;
}

static int sc_spk_set_db_locked(int db)
{
	int ret;

	if (!sc_resolve_spk())
		return -ENODEV;
	sc_maybe_snapshot_spk();
	ret = sc_set_pair(sc_spk, db);
	if (ret)
		sc_invalidate_spk();
	else {
		/* Manual set overrides the auto boost. */
		sc_spk_boost_done = true;
		sc_spk_boost_err = 0;
	}
	return ret;
}

/* NOTE: the old mic_gain/speaker_gain pair nodes were removed (v4.1):
 * FKM drives the misc frontend below, and two UIs for the same backend
 * confused users. Backend + auto boost live on via the FKM paths. */

/* Resolve the single PA enum control. Caller holds sc_lock. */
static bool sc_resolve_spk_pa(void)
{
	if (sc_spk_pa_ok && sc_spk_pa)
		return true;
	if (!sc_spk_pa)
		sc_spk_pa = sc_find_kctl(sc_spk_pa_name);
	if (!sc_spk_pa)
		return false;
	sc_spk_pa_ok = true;
	return true;
}

static ssize_t speaker_pa_gain_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	struct snd_ctl_elem_value uctl;
	int ret;

	mutex_lock(&sc_lock);
	if (!sc_resolve_spk_pa()) {
		mutex_unlock(&sc_lock);
		return -ENODEV;
	}
	memset(&uctl, 0, sizeof(uctl));
	ret = sc_spk_pa->get(sc_spk_pa, &uctl);
	mutex_unlock(&sc_lock);
	if (ret)
		return -EIO;
	return scnprintf(buf, PAGE_SIZE, "%d\n",
			(int)uctl.value.integer.value[0]);
}

static ssize_t speaker_pa_gain_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	struct snd_ctl_elem_value uctl;
	long val;
	int ret;

	if (kstrtol(buf, 10, &val))
		return -EINVAL;
	if (val < 0 || val > 7)
		return -EINVAL;
	mutex_lock(&sc_lock);
	if (!sc_resolve_spk_pa()) {
		mutex_unlock(&sc_lock);
		return -ENODEV;
	}
	memset(&uctl, 0, sizeof(uctl));
	uctl.value.integer.value[0] = val;
	ret = sc_spk_pa->put(sc_spk_pa, &uctl);
	mutex_unlock(&sc_lock);
	/* put returns 1 on change: still success, only < 0 is error. */
	return ret < 0 ? ret : (ssize_t)count;
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
			 "NeuroCore sound_control v4.1 (FKM single UI)\n");
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

static struct kobj_attribute speaker_pa_gain_attr =
	__ATTR(speaker_pa_gain, 0644, speaker_pa_gain_show,
		speaker_pa_gain_store);
static struct kobj_attribute mic_gain_stock_attr =
	__ATTR(mic_gain_stock, 0444, mic_gain_stock_show, NULL);
/* Diagnosis: resolve state + stock + boost error without needing dmesg. */
static ssize_t status_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	int ret;

	mutex_lock(&sc_lock);
	ret = scnprintf(buf, PAGE_SIZE,
			"mic_ok=%d spk_ok=%d stock_done=%d boost_done=%d boost_err=%d stock=%d/%d spk_boost_done=%d spk_boost_err=%d spk_stock=%d/%d spkpa_ok=%d\n",
			sc_mic_ok, sc_spk_ok, sc_default_done, sc_boost_done,
			sc_boost_err, sc_mic_stock_db[0], sc_mic_stock_db[1],
			sc_spk_boost_done, sc_spk_boost_err,
			sc_spk_stock_db[0], sc_spk_stock_db[1],
			sc_spk_pa_ok);
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
	&speaker_pa_gain_attr.attr,
	&mic_gain_stock_attr.attr,
	&version_attr.attr,
	&controls_attr.attr,
	&status_attr.attr,
	NULL,
};

static struct attribute_group sc_attr_group = {
	.attrs = sc_attrs,
};

/*
 * FKM (franco) compat frontend: /sys/class/misc/soundcontrol/
 * {mic_boost,speaker_l_boost,speaker_r_boost}, 0..20 like franco's own
 * driver. Show returns the last-set value (sticky) so sliders never
 * jump back; stores drive the same backend above, keeping both UIs
 * in sync.
 */
#define FKM_BOOST_MAX 20

static int fkm_mic_boost;
static int fkm_spk_l_boost;
static int fkm_spk_r_boost;

static ssize_t fkm_mic_boost_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	int ret;

	/* Best-effort boot boost (same as the old pair-node show path). */
	mutex_lock(&sc_lock);
	if (sc_resolve_mic()) {
		sc_maybe_snapshot_mic();
		sc_maybe_boost_mic();
	}
	ret = scnprintf(buf, PAGE_SIZE, "%d\n", fkm_mic_boost);
	mutex_unlock(&sc_lock);
	return ret;
}

static ssize_t fkm_mic_boost_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t size)
{
	unsigned long val;
	int ret;

	if (kstrtoul(buf, 0, &val))
		return -EINVAL;
	if (val > FKM_BOOST_MAX)
		val = FKM_BOOST_MAX;
	mutex_lock(&sc_lock);
	ret = sc_mic_set_db_locked((int)val);
	if (!ret)
		fkm_mic_boost = (int)val;
	mutex_unlock(&sc_lock);
	return ret ? ret : (ssize_t)size;
}

static ssize_t fkm_spk_l_boost_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	int ret;

	mutex_lock(&sc_lock);
	if (sc_resolve_spk()) {
		sc_maybe_snapshot_spk();
		sc_maybe_boost_spk();
	}
	ret = scnprintf(buf, PAGE_SIZE, "%d\n", fkm_spk_l_boost);
	mutex_unlock(&sc_lock);
	return ret;
}

static ssize_t fkm_spk_l_boost_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t size)
{
	unsigned long val;
	int ret;

	if (kstrtoul(buf, 0, &val))
		return -EINVAL;
	if (val > FKM_BOOST_MAX)
		val = FKM_BOOST_MAX;
	mutex_lock(&sc_lock);
	ret = sc_spk_set_db_locked((int)val);
	if (!ret)
		fkm_spk_l_boost = (int)val;
	mutex_unlock(&sc_lock);
	return ret ? ret : (ssize_t)size;
}

static ssize_t fkm_spk_r_boost_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	int ret;

	mutex_lock(&sc_lock);
	if (sc_resolve_spk()) {
		sc_maybe_snapshot_spk();
		sc_maybe_boost_spk();
	}
	ret = scnprintf(buf, PAGE_SIZE, "%d\n", fkm_spk_r_boost);
	mutex_unlock(&sc_lock);
	return ret;
}

static ssize_t fkm_spk_r_boost_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t size)
{
	unsigned long val;
	int ret;

	if (kstrtoul(buf, 0, &val))
		return -EINVAL;
	if (val > FKM_BOOST_MAX)
		val = FKM_BOOST_MAX;
	mutex_lock(&sc_lock);
	ret = sc_spk_set_db_locked((int)val);
	if (!ret)
		fkm_spk_r_boost = (int)val;
	mutex_unlock(&sc_lock);
	return ret ? ret : (ssize_t)size;
}

static DEVICE_ATTR(mic_boost, 0644, fkm_mic_boost_show, fkm_mic_boost_store);
static DEVICE_ATTR(speaker_l_boost, 0644, fkm_spk_l_boost_show,
		   fkm_spk_l_boost_store);
static DEVICE_ATTR(speaker_r_boost, 0644, fkm_spk_r_boost_show,
		   fkm_spk_r_boost_store);

static struct attribute *fkm_soundcontrol_attrs[] = {
	&dev_attr_mic_boost.attr,
	&dev_attr_speaker_l_boost.attr,
	&dev_attr_speaker_r_boost.attr,
	NULL,
};

static struct attribute_group fkm_soundcontrol_group = {
	.attrs = fkm_soundcontrol_attrs,
};

static struct miscdevice fkm_soundcontrol_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "soundcontrol",
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
	/* FKM frontend is best-effort: never fail sound init for it. */
	if (misc_register(&fkm_soundcontrol_device)) {
		pr_err("sound_control: fkm misc_register failed\n");
	} else if (sysfs_create_group(
			&fkm_soundcontrol_device.this_device->kobj,
			&fkm_soundcontrol_group)) {
		pr_err("sound_control: fkm sysfs group failed\n");
		misc_deregister(&fkm_soundcontrol_device);
	} else {
		pr_info("sound_control: fkm frontend ready (/sys/class/misc/soundcontrol)\n");
	}
	pr_info("sound_control: ready (/sys/kernel/sound_control)\n");

	/* Try early resolve; first sysfs access retries if audio is down. */
	mutex_lock(&sc_lock);
	if (sc_resolve_mic()) {
		sc_maybe_snapshot_mic();
		sc_maybe_boost_mic();
	}
	sc_resolve_spk();
	if (sc_spk_ok) {
		sc_maybe_snapshot_spk();
		sc_maybe_boost_spk();
	}
	mutex_unlock(&sc_lock);
	return 0;
}
late_initcall(sound_control_init);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("NeuroCore");
MODULE_DESCRIPTION("Bolero TX/RX digital gain control (dB)");
