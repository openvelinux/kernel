// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hugetlb backgreound cleaning support.
 * (C) Li Zhe, April 2025
 */
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>
#include <linux/node.h>
#include "hugetlb_background_clean.h"

/* cpu which can do the cleaning job */
static struct cpumask hugetlb_background_clr_cpumask;

static ssize_t hugetlb_busy_cpu_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	cpumask_var_t tmpmask;
	ssize_t ret;

	if (!alloc_cpumask_var(&tmpmask, GFP_KERNEL))
		return -ENOMEM;

	cpumask_xor(tmpmask, &hugetlb_background_clr_cpumask, cpu_possible_mask);
	ret = sprintf(buf, "%*pb\n", cpumask_pr_args(tmpmask));
	free_cpumask_var(tmpmask);

	return ret;
}

static DEFINE_MUTEX(hugetlb_clean_mask_lock);
static ssize_t _hugetlb_busy_write(const char *buffer)
{
	ssize_t ret;
	cpumask_var_t tmpmask;

	if (buffer == NULL)
		return -EINVAL;
	if (!alloc_cpumask_var(&tmpmask, GFP_KERNEL))
		return -ENOMEM;

	ret = cpulist_parse(buffer, tmpmask);
	if (ret < 0 || cpumask_last(tmpmask) >= nr_cpu_ids) {
		pr_warn("get incorrect CPU range\n");
		goto out_tmpmask;
	}

	mutex_lock(&hugetlb_clean_mask_lock);
	cpumask_and(tmpmask, &hugetlb_background_clr_cpumask, tmpmask);
	cpumask_xor(&hugetlb_background_clr_cpumask, &hugetlb_background_clr_cpumask, tmpmask);
	mutex_unlock(&hugetlb_clean_mask_lock);

out_tmpmask:
	free_cpumask_var(tmpmask);
	return ret;
}

static ssize_t hugetlb_busy_cpu_store(struct kobject *kobj,
			struct kobj_attribute *attr, const char *buf, size_t len)
{
	ssize_t err;

	err = _hugetlb_busy_write(buf);
	if (err == 0)
		err = len;

	return err;
}

static struct kobj_attribute hugetlb_busy_cpu_attr =
	__ATTR(hugetlb_busy_cpu, 0644, hugetlb_busy_cpu_show, hugetlb_busy_cpu_store);

static ssize_t hugetlb_free_cpu_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%*pb\n",
		cpumask_pr_args(&hugetlb_background_clr_cpumask));
}

static ssize_t _hugetlb_free_write(const char *buffer)
{
	ssize_t ret;
	cpumask_var_t tmpmask;

	if (buffer == NULL)
		return -EINVAL;
	if (!alloc_cpumask_var(&tmpmask, GFP_KERNEL))
		return -ENOMEM;

	ret = cpulist_parse(buffer, tmpmask);
	if (ret < 0 || cpumask_last(tmpmask) >= nr_cpu_ids) {
		pr_warn("get incorrect CPU range\n");
		goto out_tmpmask;
	}

	mutex_lock(&hugetlb_clean_mask_lock);
	cpumask_andnot(tmpmask, tmpmask, &hugetlb_background_clr_cpumask);
	cpumask_or(&hugetlb_background_clr_cpumask, &hugetlb_background_clr_cpumask, tmpmask);
	mutex_unlock(&hugetlb_clean_mask_lock);

out_tmpmask:
	free_cpumask_var(tmpmask);
	return ret;
}

static ssize_t hugetlb_free_cpu_store(struct kobject *kobj,
			struct kobj_attribute *attr, const char *buf, size_t len)
{
	ssize_t err;

	err = _hugetlb_free_write(buf);
	if (err == 0)
		err = len;

	return err;
}

static struct kobj_attribute hugetlb_free_cpu_attr =
	__ATTR(hugetlb_free_cpu, 0644, hugetlb_free_cpu_show, hugetlb_free_cpu_store);

static struct attribute *hugetlb_attrs[] = {
	&hugetlb_busy_cpu_attr.attr,
	&hugetlb_free_cpu_attr.attr,
	NULL,
};

static const struct attribute_group hugetlb_attr_group = {
	.attrs = hugetlb_attrs,
};

static int __init background_cleaning_init(void)
{
	struct kobject *hugetlb_kobj;
	int err;

	hugetlb_kobj = kobject_create_and_add("hugetlb", mm_kobj);
	if (!hugetlb_kobj) {
		pr_err("HugeTLB: Unable to create hugetlb dir");
		err = -ENOMEM;
	} else {
		err = sysfs_create_group(hugetlb_kobj, &hugetlb_attr_group);
		if (err) {
			pr_err("HugeTLB: Unable to create group");
			kobject_put(hugetlb_kobj);
		}
	}

	return err;
}
late_initcall(background_cleaning_init);
