// SPDX-License-Identifier: GPL-2.0+
/*
 * inspector.c - inspector support
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

#define pr_fmt(fmt) "CPUINSPECT: " fmt

#include <linux/cpu.h>
#include <linux/cpuinspect.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/pm_qos.h>

#include "cpuinspect.h"


char param_inspector[CPUINSPECT_NAME_LEN];

LIST_HEAD(cpu_inspectors);
struct cpu_inspector *curr_cpu_inspector;
struct cpu_inspector *prev_cpu_inspector;

/**
 * cpuinspect_find_inspector - finds a inspector of the specified name
 * @str: the name
 */
static struct cpu_inspector *cpuinspect_find_inspector(const char *str)
{
	struct cpu_inspector *insp;

	list_for_each_entry(insp, &cpu_inspectors, list)
		if (!strncasecmp(str, insp->name, CPUINSPECT_NAME_LEN))
			return insp;
	return NULL;
}

/**
 * cpuinspect_switch_inspector - changes the inspector
 * @insp: the new target inspector
 */
int cpuinspect_switch_inspector(struct cpu_inspector *insp)
{
	if (!insp)
		return -EINVAL;

	if (insp == curr_cpu_inspector)
		return 0;

	curr_cpu_inspector = insp;
	pr_info("using inspector %s, group_num: %lu\n", insp->name, insp->group_num);

	return 0;
}

/**
 * cpuinspect_register_inspector - registers a inspector
 * @insp: the inspector
 */
int cpuinspect_register_inspector(struct cpu_inspector *insp)
{
	int ret = -EEXIST;

	if (!insp)
		return -EINVAL;

	mutex_lock(&cpuinspect_lock);
	if (cpuinspect_find_inspector(insp->name) == NULL) {
		ret = 0;
		list_add_tail(&insp->list, &cpu_inspectors);

		/*
		 * We select the inspector if current inspector is NULL or it is
		 * one specificed by kernel parameter.
		 */
		if (!curr_cpu_inspector ||
		    !strncasecmp(param_inspector, insp->name, CPUINSPECT_NAME_LEN))
			cpuinspect_switch_inspector(insp);
	}
	mutex_unlock(&cpuinspect_lock);

	return ret;
}
EXPORT_SYMBOL(cpuinspect_register_inspector);

/**
 * cpuinspect_unregister_inspector - unregisters a inspector
 * @insp: the inspector
 */
int cpuinspect_unregister_inspector(struct cpu_inspector *insp)
{
	if (!insp)
		return -EINVAL;

	mutex_lock(&cpuinspect_lock);
	if (curr_cpu_inspector == insp) {
		if (ci_core.inspect_on) {
			mutex_unlock(&cpuinspect_lock);
			return -EBUSY;
		}

		curr_cpu_inspector = NULL;
	}

	if (!list_empty(&insp->list))
		list_del(&insp->list);

	mutex_unlock(&cpuinspect_lock);

	return 0;
}
EXPORT_SYMBOL(cpuinspect_unregister_inspector);
