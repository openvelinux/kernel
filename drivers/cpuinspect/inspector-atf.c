// SPDX-License-Identifier: GPL-2.0+
/*
 * inspector-atf: cpuinspect inspector for EFI-based systems
 *
 * Copyright (c) Huawei Technologies Co., Ltd. 2022-2023. All rights reserved.
 *
 * Author: Yu Liao <liaoyu15@huawei.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/cpuinspect.h>
#include <linux/arm-smccc.h>

#define PRIVATE_ARM_SMC_ID_STL_GET_MAX_GROUP	0x83000504
#define PRIVATE_ARM_SMC_ID_STL_ONLINE_TEST	0x83000505
#define CPUINSPECT_NOT_SUPPORTED		-1

static struct cpu_inspector atf_inspector;

static int atf_get_group_num(void)
{
	struct arm_smccc_res res;

	arm_smccc_smc(PRIVATE_ARM_SMC_ID_STL_GET_MAX_GROUP, 0, 0, 0, 0,
			0, 0, 0, &res);

	return res.a0;
}

static int atf_run_chip_test(unsigned int group)
{
	struct arm_smccc_res res;

	arm_smccc_smc(PRIVATE_ARM_SMC_ID_STL_ONLINE_TEST, group, 0, 0, 0,
			0, 0, 0, &res);

	return res.a0;
}

static struct cpu_inspector atf_inspector = {
	.name		= "atf",
	.start_inspect	= atf_run_chip_test,
};

/**
 * init_atf_inspector - initializes the inspector
 */
static __init int init_atf_inspector(void)
{
	unsigned long ret;

	ret = atf_get_group_num();
	if (ret == CPUINSPECT_NOT_SUPPORTED) {
		pr_info("BIOS does not support CPU inspect.\nFailed to register inspector %s\n",
			atf_inspector.name);
		return -EOPNOTSUPP;
	}

	atf_inspector.group_num = ret;

	return cpuinspect_register_inspector(&atf_inspector);
}

static __exit void exit_atf_inspector(void)
{
	cpuinspect_unregister_inspector(&atf_inspector);
}

MODULE_LICENSE("GPL");
module_init(init_atf_inspector);
module_exit(exit_atf_inspector);
