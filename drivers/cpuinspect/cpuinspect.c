// SPDX-License-Identifier: GPL-2.0+
/*
 * cpuinspect.c - core cpuinspect infrastructure
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

#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/cpu.h>
#include <linux/cpuinspect.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/atomic.h>

#include "cpuinspect.h"

#define CPUINSPECT_SLEEP_TIMEOUT	1000000UL
/*
 * The core struct, store the most relevant data for cpuinspect.
 */
struct cpuinspect ci_core = {
	.inspect_times	= 1,
	.cpu_utility	= 90,
};

static struct task_struct *cpuinspect_threads[NR_CPUS];
static atomic_t active_threads_num;
DECLARE_BITMAP(result, NR_CPUS);
DEFINE_MUTEX(cpuinspect_lock);

/* inspection thread function */
static int run_inspector(void *data)
{
	unsigned int inspect_times = 0, group, ret;
	unsigned int cpu = (unsigned long)data;
	ktime_t start_time, duration;
	unsigned long sleep_us;

	while (!kthread_should_stop()) {
		if (inspect_times >= ci_core.inspect_times || !cpu_online(cpu))
			break;

		for (group = 0; group < curr_cpu_inspector->group_num; group++) {
			start_time = ktime_get();
			ret = curr_cpu_inspector->start_inspect(group);
			if (ret) {
				set_bit(cpu, result);
				cpuinspect_result_notify();
			}

			/*
			 * Sleep for a while if user set desired cpu utility.
			 */
			duration = ktime_get() - start_time;
			sleep_us = (duration * 100 / ci_core.cpu_utility - duration) / 1000;
			/*
			 * During low cpu utility in cpu inspect we might wait a
			 * while; let's avoid the hung task warning.
			 */
			sleep_us = min(sleep_us, CPUINSPECT_SLEEP_TIMEOUT);
			/*
			 * Since usleep_range is built on top of hrtimers,
			 * and we don't want to introduce a large number of
			 * undesired interrupts, choose a range of 200us
			 * to balance performance and latency. This can
			 * cause inspection threads cpu utility is lower
			 * than required cpu utility. And this also prevents
			 * soft lockup.
			 */
			usleep_range(sleep_us, sleep_us + 200);
		}
		inspect_times++;
	}

	cpuinspect_threads[cpu] = NULL;
	/*
	 * When this condition is met, it indicate this is the final cpuinspect
	 * thread, mark inspect state as 0 and notify user that it has been
	 * completed.
	 */
	if (atomic_dec_and_test(&active_threads_num)) {
		ci_core.inspect_on = 0;
		cpuinspect_result_notify();
	}

	return 0;
}

int start_inspect_threads(void)
{
	unsigned int cpu = 0;

	bitmap_zero(result, NR_CPUS);

	ci_core.inspect_on = 1;
	for_each_cpu(cpu, &ci_core.inspect_cpumask) {
		cpuinspect_threads[cpu] = kthread_create_on_node(run_inspector,
					(void *)(unsigned long)cpu,
					cpu_to_node(cpu), "cpuinspect/%u", cpu);
		if (IS_ERR(cpuinspect_threads[cpu])) {
			cpuinspect_threads[cpu] = NULL;
			continue;
		}

		kthread_bind(cpuinspect_threads[cpu], cpu);
		wake_up_process(cpuinspect_threads[cpu]);
		atomic_inc(&active_threads_num);
	}

	/*
	 * If creating inspection threads for all CPUs in mask fails (or
	 * inspect_cpumask is empty), notify user, mark the inspection status
	 * as 0 and simply exit.
	 */
	if (unlikely(!atomic_read(&active_threads_num))) {
		ci_core.inspect_on = 0;
		cpuinspect_result_notify();
	}

	return 0;
}

int stop_inspect_threads(void)
{
	unsigned int cpu = 0;

	/* All inspection threads has been stopped */
	if (atomic_read(&active_threads_num) == 0)
		return 0;

	for_each_cpu(cpu, &ci_core.inspect_cpumask) {
		if (cpuinspect_threads[cpu])
			kthread_stop(cpuinspect_threads[cpu]);
	}

	return 0;
}

/**
 * cpuinspect_init - core initializer
 */
static int __init cpuinspect_init(void)
{
	cpumask_copy(&ci_core.inspect_cpumask, cpu_all_mask);

	return cpuinspect_add_interface();
}

static void __exit cpuinspect_exit(void)
{
	return cpuinspect_remove_interface();
}

module_init(cpuinspect_init);
module_exit(cpuinspect_exit);
module_param_string(inspector, param_inspector, CPUINSPECT_NAME_LEN, 0444);
MODULE_LICENSE("GPL");
