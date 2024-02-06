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

/* HIP09 */
#define AIDR_EL1_DVMBM_MASK	GENMASK_ULL(13, 12)
#define SYS_LSUDVM_CTRL_EL2	sys_reg(3, 4, 15, 7, 4)
#define LSUDVM_CTLR_EL2_MASK	BIT_ULL(0)

void probe_hisi_cpu_type(void);
bool hisi_ncsnp_supported(void);
bool hisi_dvmbm_supported(void);

#endif /* __HISI_VIRT_H__ */
