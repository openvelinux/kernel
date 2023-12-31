// SPDX-License-Identifier: GPL-2.0+
/*
 * sysfs.c - sysfs support
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
#include <linux/cpuinspect.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/completion.h>
#include <linux/capability.h>
#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/printk.h>
#include <linux/bitmap.h>

#include "cpuinspect.h"

static ssize_t available_inspector_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct cpu_inspector *tmp;
	ssize_t i = 0;

	list_for_each_entry(tmp, &cpu_inspectors, list) {
		if (i >= (ssize_t) (PAGE_SIZE - (CPUINSPECT_NAME_LEN + 2)))
			goto out;

		i += scnprintf(&buf[i], CPUINSPECT_NAME_LEN + 1, "%s ", tmp->name);
	}

out:
	i += sprintf(&buf[i], "\n");
	return i;
}

static ssize_t current_inspector_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	ssize_t ret;

	if (curr_cpu_inspector)
		ret = sprintf(buf, "%s\n", curr_cpu_inspector->name);
	else
		ret = sprintf(buf, "none\n");

	return ret;
}

static ssize_t current_inspector_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	char insp_name[CPUINSPECT_NAME_LEN + 1];
	int ret;
	struct cpu_inspector *insp;

	ret = sscanf(buf, "%" __stringify(CPUINSPECT_NAME_LEN) "s", insp_name);
	if (ret != 1)
		return -EINVAL;

	if (ci_core.inspect_on)
		return -EBUSY;

	ret = -EINVAL;
	list_for_each_entry(insp, &cpu_inspectors, list) {
		if (!strncmp(insp->name, insp_name, CPUINSPECT_NAME_LEN)) {
			ret = cpuinspect_switch_inspector(insp);
			break;
		}
	}

	return ret ? ret : count;
}

ssize_t patrol_complete_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	return sprintf(buf, "%d\n", !ci_core.inspect_on);
}

ssize_t cpu_utility_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	return sprintf(buf, "%u\n", ci_core.cpu_utility);
}

ssize_t cpu_utility_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t size)
{
	unsigned int cpu_util;

	if (kstrtouint(buf, 10, &cpu_util) || cpu_util < 1 ||  cpu_util > 100)
		return -EINVAL;

	ci_core.cpu_utility = cpu_util;

	return size;
}

ssize_t patrol_times_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	return sprintf(buf, "%lu\n", ci_core.inspect_times);
}

ssize_t patrol_times_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t size)
{
	/*
	 * It is not allowed to modify patrol times during the CPU
	 * inspection operation.
	 */
	if (ci_core.inspect_on)
		return -EBUSY;

	if (kstrtoul(buf, 10, &ci_core.inspect_times))
		return -EINVAL;

	return size;
}

ssize_t start_patrol_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t size)
{
	bool start_patrol = false;

	if (strtobool(buf, &start_patrol) < 0)
		return -EINVAL;

	if (!mutex_trylock(&cpuinspect_lock))
		return -EBUSY;

	/*
	 * It is not allowed to start the inspection again during the
	 * inspection process.
	 */
	if (start_patrol && (int) start_patrol == ci_core.inspect_on) {
		mutex_unlock(&cpuinspect_lock);
		return -EBUSY;
	}

	if (start_patrol == 0)
		stop_inspect_threads();
	else if (curr_cpu_inspector)
		start_inspect_threads();

	mutex_unlock(&cpuinspect_lock);
	return size;
}

static ssize_t result_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	return sprintf(buf, "%*pbl\n", nr_cpu_ids, &result);
}

static ssize_t cpumask_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	return sprintf(buf, "%*pbl\n",
		       cpumask_pr_args(&ci_core.inspect_cpumask));
}

static ssize_t cpumask_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	ssize_t err;

	/*
	 * It is not allowed to modify cpumask during the CPU
	 * inspection operation.
	 */
	if (ci_core.inspect_on)
		return -EBUSY;

	err = cpulist_parse(buf, &ci_core.inspect_cpumask);
	if (err)
		return err;

	return count;
}

/*
 * Tell userspace to handle result if one of the following conditions is met:
 *	- Found fault core
 *	- Inspection task completed
 */
void cpuinspect_result_notify(void)
{
	struct device *dev_root = bus_get_dev_root(&cpu_subsys);

	if (dev_root) {
		sysfs_notify(&dev_root->kobj, "cpuinspect", "result");
		put_device(dev_root);
	}
}

static DEVICE_ATTR_RO(result);
static DEVICE_ATTR_WO(start_patrol);
static DEVICE_ATTR_RO(patrol_complete);
static DEVICE_ATTR_RW(cpu_utility);
static DEVICE_ATTR_RW(cpumask);
static DEVICE_ATTR_RW(patrol_times);

/* show and switch inspector */
static DEVICE_ATTR_RO(available_inspector);
static DEVICE_ATTR_RW(current_inspector);


static struct attribute *cpuinspect_attrs[] = {
	&dev_attr_result.attr,
	&dev_attr_start_patrol.attr,
	&dev_attr_patrol_complete.attr,
	&dev_attr_cpu_utility.attr,
	&dev_attr_cpumask.attr,
	&dev_attr_patrol_times.attr,
	&dev_attr_available_inspector.attr,
	&dev_attr_current_inspector.attr,
	NULL
};

static struct attribute_group cpuinspect_attr_group = {
	.attrs = cpuinspect_attrs,
	.name = "cpuinspect",
};

/**
 * cpuinspect_add_interface - add CPU global sysfs attributes
 * @dev: the target device
 */
int cpuinspect_add_interface(void)
{
	struct device *dev_root = bus_get_dev_root(&cpu_subsys);
	int retval;

	if (!dev_root)
		return -EINVAL;

	retval = sysfs_create_group(&dev_root->kobj, &cpuinspect_attr_group);
	put_device(dev_root);
	return retval;
}

/**
 * cpuinspect_remove_interface - remove CPU global sysfs attributes
 * @dev: the target device
 */
void cpuinspect_remove_interface(void)
{
	struct device *dev_root = bus_get_dev_root(&cpu_subsys);

	if (dev_root) {
		sysfs_remove_group(&dev_root->kobj, &cpuinspect_attr_group);
		put_device(dev_root);
	}
}
