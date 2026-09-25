/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 4.14 backport: the unified <linux/unaligned.h> header only exists
 * upstream on newer kernels (this tree uses <asm/unaligned.h>).
 * Added for the zstd 1.5.7 import which includes it.
 */
#ifndef _LINUX_UNALIGNED_H
#define _LINUX_UNALIGNED_H

#include <asm/unaligned.h>

#endif /* _LINUX_UNALIGNED_H */
