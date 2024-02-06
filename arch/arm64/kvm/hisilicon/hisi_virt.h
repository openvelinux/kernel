/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright(c) 2022 Huawei Technologies Co., Ltd
 */

#ifndef __HISI_VIRT_H__
#define __HISI_VIRT_H__

enum hisi_cpu_type {
	HI_1612,
	HI_1616,
	HI_1620,
	HI_IP09,
	UNKNOWN_HI_TYPE
};

void probe_hisi_cpu_type(void);
bool hisi_ncsnp_supported(void);

#endif /* __HISI_VIRT_H__ */
