#ifndef _ADRENO_IDLER_H
#define _ADRENO_IDLER_H

#include <linux/devfreq.h>

#ifdef CONFIG_ADRENO_IDLER
int adreno_idler_check(struct devfreq *devfreq, int level,
		       struct devfreq_dev_status *stats);
void adreno_idler_set_active(int active);
#else
static inline int adreno_idler_check(struct devfreq *devfreq, int level,
				     struct devfreq_dev_status *stats)
{
	return level;
}
#endif

#endif /* _ADRENO_IDLER_H */
