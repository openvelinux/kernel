/* SPDX-License-Identifier: GPL-2.0-or-later WITH Linux-syscall-note */
/* Copyright (c) 2024-2024 HiSilicon Limited. */
#ifndef _UAPI_HISI_SOC_CACHE_H
#define _UAPI_HISI_SOC_CACHE_H

#include <linux/types.h>

/* HISI_CACHE_MAINTAIN: cache maintain operation for HiSilicon SoC */
#define HISI_CACHE_MAINTAIN	_IOW('C', 1, unsigned long)

/*
 * Further information of these operations can be found at:
 * https://developer.arm.com/documentation/ihi0050/latest/
 */
enum hisi_soc_cache_maint_type {
	HISI_CACHE_MAINT_CLEANSHARED,
	HISI_CACHE_MAINT_CLEANINVALID,
	HISI_CACHE_MAINT_MAKEINVALID,

	HISI_CACHE_MAINT_MAX
};

/**
 * struct hisi_soc_cache_ioctl_param - User data for hisi cache operates.
 * @op_type: cache maintain type
 * @addr: cache maintain address
 * @size: cache maintain size
 */
struct hisi_soc_cache_ioctl_param {
	enum hisi_soc_cache_maint_type op_type;
	unsigned long addr;
	unsigned long size;
};

#endif
