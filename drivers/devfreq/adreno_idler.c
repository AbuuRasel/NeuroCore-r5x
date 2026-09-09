/* Adreno idler - GPU battery saver for msm-adreno-tz
 *
 * Ramps the GPU down to its lowest frequency when it has been idle for a
 * sustained period, instead of letting the TrustZone governor hover at
 * intermediate levels. Any busy poll resets the idle counter immediately,
 * so interactive workloads are unaffected.
 *
 * Tunables (via /sys/module/adreno_idler/parameters/):
 *  adreno_idler_active          - master switch (default 1)
 *  adreno_idler_downdifferential - busy% at/below which a poll counts as
 *                                  idle (default 50)
 *  adreno_idler_idlewait        - consecutive idle polls before clamping
 *                                  (default 12)
 *  adreno_idler_idleworkload    - busy_time (us) below which a poll always
 *                                  counts as idle (default 6000)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2.
 */
#include <linux/module.h>
#include <linux/devfreq.h>

#include "adreno_idler.h"

static int adreno_idler_active = 1;
static int adreno_idler_downdifferential = 50;
static int adreno_idler_idlewait = 12;
static int adreno_idler_idleworkload = 6000;
static unsigned int adreno_idler_idlecount;

module_param_named(adreno_idler_active, adreno_idler_active, int, 0664);
module_param_named(adreno_idler_downdifferential, adreno_idler_downdifferential,
		   int, 0664);
module_param_named(adreno_idler_idlewait, adreno_idler_idlewait, int, 0664);
module_param_named(adreno_idler_idleworkload, adreno_idler_idleworkload,
		   int, 0664);

int adreno_idler_check(struct devfreq *devfreq, int level,
		       struct devfreq_dev_status *stats)
{
	unsigned int max_level;
	bool idle = false;

	if (!adreno_idler_active || !devfreq || !devfreq->profile ||
	    !devfreq->profile->max_state)
		return level;

	max_level = devfreq->profile->max_state - 1;

	if (stats && stats->total_time) {
		unsigned int dd = (unsigned int)adreno_idler_downdifferential;

		if (dd > 100)
			dd = 100;
		/* busy% <= dd  -> idle poll (overflow-safe) */
		if (stats->busy_time <= stats->total_time / 100 * dd)
			idle = true;
		/* tiny absolute workload -> idle poll regardless */
		if (stats->busy_time <
		    (unsigned long)adreno_idler_idleworkload)
			idle = true;
	}

	if (!idle) {
		adreno_idler_idlecount = 0;
		return level;
	}

	if (++adreno_idler_idlecount < (unsigned int)adreno_idler_idlewait)
		return level;

	/* sustained idle: pin to the slowest level */
	return (int)max_level;
}
