// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * LRNG auxiliary interfaces
 *
 * Copyright (C) 2019 Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2017 Jason A. Donenfeld <Jason@zx2c4.com>. All
 * Rights Reserved.
 * Copyright (C) 2016 Jason Cooper <jason@lakedaemon.net>
 */

#include <linux/mm.h>
#include <linux/random.h>

#include "lrng_internal.h"

struct batched_entropy {
	union {
		u64 entropy_u64[LRNG_DRNG_BLOCKSIZE / sizeof(u64)];
		u32 entropy_u32[LRNG_DRNG_BLOCKSIZE / sizeof(u32)];
	};
	unsigned int position;
	spinlock_t batch_lock;
};

/*
 * Get a random word for internal kernel use only. The quality of the random
 * number is either as good as RDRAND or as good as /dev/urandom, with the
 * goal of being quite fast and not depleting entropy.
 */
static DEFINE_PER_CPU(struct batched_entropy, batched_entropy_u64) = {
	.batch_lock	= __SPIN_LOCK_UNLOCKED(batched_entropy_u64.lock),
};

u64 get_random_u64(void)
{
	u64 ret;
	unsigned long flags;
	struct batched_entropy *batch;

#if BITS_PER_LONG == 64
	if (arch_get_random_long((unsigned long *)&ret))
		return ret;
#else
	if (arch_get_random_long((unsigned long *)&ret) &&
	    arch_get_random_long((unsigned long *)&ret + 1))
		return ret;
#endif

	lrng_debug_report_seedlevel("get_random_u64");

	batch = raw_cpu_ptr(&batched_entropy_u64);
	spin_lock_irqsave(&batch->batch_lock, flags);
	if (batch->position % ARRAY_SIZE(batch->entropy_u64) == 0) {
		lrng_sdrng_get_atomic((u8 *)batch->entropy_u64,
				      LRNG_DRNG_BLOCKSIZE);
		batch->position = 0;
	}
	ret = batch->entropy_u64[batch->position++];
	spin_unlock_irqrestore(&batch->batch_lock, flags);
	return ret;
}
EXPORT_SYMBOL(get_random_u64);

static DEFINE_PER_CPU(struct batched_entropy, batched_entropy_u32) = {
	.batch_lock	= __SPIN_LOCK_UNLOCKED(batched_entropy_u32.lock),
};

u32 get_random_u32(void)
{
	u32 ret;
	unsigned long flags;
	struct batched_entropy *batch;

	if (arch_get_random_int(&ret))
		return ret;

	lrng_debug_report_seedlevel("get_random_u32");

	batch = raw_cpu_ptr(&batched_entropy_u32);
	spin_lock_irqsave(&batch->batch_lock, flags);
	if (batch->position % ARRAY_SIZE(batch->entropy_u32) == 0) {
		lrng_sdrng_get_atomic((u8 *)batch->entropy_u32,
				      LRNG_DRNG_BLOCKSIZE);
		batch->position = 0;
	}
	ret = batch->entropy_u32[batch->position++];
	spin_unlock_irqrestore(&batch->batch_lock, flags);
	return ret;
}
EXPORT_SYMBOL(get_random_u32);

/*
 * It's important to invalidate all potential batched entropy that might
 * be stored before the crng is initialized, which we can do lazily by
 * simply resetting the counter to zero so that it's re-extracted on the
 * next usage.
 */
void invalidate_batched_entropy(void)
{
	int cpu;
	unsigned long flags;

	for_each_possible_cpu(cpu) {
		struct batched_entropy *batched_entropy;

		batched_entropy = per_cpu_ptr(&batched_entropy_u32, cpu);
		spin_lock_irqsave(&batched_entropy->batch_lock, flags);
		batched_entropy->position = 0;
		spin_unlock(&batched_entropy->batch_lock);

		batched_entropy = per_cpu_ptr(&batched_entropy_u64, cpu);
		spin_lock(&batched_entropy->batch_lock);
		batched_entropy->position = 0;
		spin_unlock_irqrestore(&batched_entropy->batch_lock, flags);
	}
}

/* 4.14 backport: randomize_page() already lives in mm/util.c on this
 * tree (upstream moved it into LRNG only in 5.x); drop the duplicate. */

/***************************** CPU hotplug *********************************
 * 4.14 core (kernel/cpu.c) expects random_prepare_cpu/random_online_cpu;
 * with random.o gone, LRNG serves them: invalidate per-CPU batches so
 * fresh randomness is served after CPU onlining. */
int __cold random_prepare_cpu(unsigned int cpu)
{
	per_cpu_ptr(&batched_entropy_u32, cpu)->position = UINT_MAX;
	per_cpu_ptr(&batched_entropy_u64, cpu)->position = UINT_MAX;
	return 0;
}

int __cold random_online_cpu(unsigned int cpu)
{
	/* No LRNG per-CPU IRQ state needs resetting here. */
	return 0;
}
