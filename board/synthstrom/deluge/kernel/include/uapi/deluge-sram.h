/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Userspace ABI for /dev/deluge-sram — a fixed on-chip SRAM carveout the
 * Synthstrom Deluge hands to userspace (the Spark synth) as cached, CPU-only
 * scratch for hot DSP data. mmap() the region (whole thing at offset 0);
 * DELUGE_SRAM_GET_INFO reports its physical base and size so userspace need
 * not hardcode the layout.
 */
#ifndef _UAPI_LINUX_DELUGE_SRAM_H
#define _UAPI_LINUX_DELUGE_SRAM_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct deluge_sram_info {
	__u32 phys;	/* physical base address of the region */
	__u32 size;	/* region size in bytes */
};

#define DELUGE_SRAM_IOC_MAGIC	'D'
#define DELUGE_SRAM_GET_INFO	_IOR(DELUGE_SRAM_IOC_MAGIC, 1, struct deluge_sram_info)

#endif /* _UAPI_LINUX_DELUGE_SRAM_H */
