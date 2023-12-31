/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cpuinspect.h - a generic framework for CPU online inspection
 *
 * Copyright (c) 2023 Yu Liao <liaoyu15@huawei.com>
 */

#ifndef __LINUX_CPUINSPECT_H
#define __LINUX_CPUINSPECT_H

#include <linux/percpu.h>
#include <linux/list.h>
#include <linux/spinlock.h>

#define CPUINSPECT_NAME_LEN 16

/**
 * struct cpu_inspector - CPU Inspection driver. Inspection code may run in
 *	kernel, BIOS, trusted OS, etc., and contains many test cases. All test
 *	cases can be divided into multiple or one group, provied a function
 *	to start inspection for a specified group.
 *
 * @name:		Pointer to inspector name
 * @list:		List head for registration (internal)
 * @group_num:		Number of inspection code groups
 * @start_inspect:	Function to start inspect process, passes group
 *			number as a argument
 */
struct cpu_inspector {
	const char		name[CPUINSPECT_NAME_LEN];
	struct list_head	list;
	unsigned long		group_num;

	int			(*start_inspect)(unsigned int group);
};

extern int cpuinspect_register_inspector(struct cpu_inspector *insp);
extern int cpuinspect_unregister_inspector(struct cpu_inspector *insp);

#endif /* __LINUX_CPUINSPECT_H */
