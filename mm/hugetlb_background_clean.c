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

static void khzerod_run_mask(const struct cpumask *cpumask)
{
	int node;

	for_each_node(node) {
		if (cpumask_intersects(cpumask_of_node(node), cpumask))
			khzerod_run(node);
	}
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

	khzerod_run_mask(&hugetlb_background_clr_cpumask);

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
	khzerod_run_mask(tmpmask);
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

struct khzerod_info {
	struct hstate *h;
	int nid;
};

/*
 * Mutex to protect prezero kthread stopping / starting.
 * This is really per <hstate,nid> tuple, but since this
 * only happens when prezeroing is enabled/disabled via
 * sysfs (rarely), a mutex per <hstate,nid> would be
 * overkill.
 */
static DEFINE_MUTEX(prezero_chg_lock);

/*
 * This per-node-per-hstate kthread pre-zeroes pages that are on
 * the freelist. They remain on the freelist while this is being
 * done. When pre-zeroing is done, they are moved to the head
 * of the list.
 *
 * Pages are left on the freelist because an allocation should not
 * fail just because the page is being prezeroed. In the rare
 * corner case that a page that is being worked on by this
 * thread is taken as part of an allocation, the caller will
 * wait for the prezero to finish (see hpage_wait_zerobusy).
 */
static int khzerod(void *p)
{
	struct khzerod_info *ki = p;
	struct hstate *h = ki->h;
	const int nid = ki->nid;
	const struct cpumask *cpumask = cpumask_of_node(nid);
	int cpu = smp_processor_id();
	struct cpumask tmp_mask;
	struct page *page;
	struct list_head *freelist;

	if (!cpumask_empty(cpumask))
		set_cpus_allowed_ptr(current, cpumask);

	freelist = &h->hugepage_freelists[nid];

	while (1) {
		cond_resched();
		if (kthread_should_stop())
			break;

		cpu = smp_processor_id();
		if (unlikely(!cpumask_test_cpu(cpu, &hugetlb_background_clr_cpumask))) {

			wait_event_interruptible(h->bc.hzerod_wait[nid],
					cpumask_and(&tmp_mask, &hugetlb_background_clr_cpumask,
						cpumask));

			if (set_cpus_allowed_ptr(current, &tmp_mask)) {
				pr_err("set_cpus_allowed_ptr fail, nid %d\n", nid);
				break;
			}

			continue;
		}

		/*
		 * fast path: we don't acquire lock here, which means we need to
		 * double check after getting the lock
		 */
		if ((h->bc.free_huge_pages_zero_node[nid] == h->free_huge_pages_node[nid])
				|| list_empty(freelist)) {
			wait_event_interruptible(h->bc.hzerod_wait[nid],
					(h->bc.free_huge_pages_zero_node[nid] !=
						h->free_huge_pages_node[nid]) &&
					!list_empty(freelist));
			continue;
		}

		spin_lock_irq(&hugetlb_lock);

		if (list_empty(freelist)) {
			spin_unlock_irq(&hugetlb_lock);
			continue;
		}

		page = list_last_entry(freelist, struct page, lru);
		if (HPagePreZeroed(page)) {
			spin_unlock_irq(&hugetlb_lock);
			continue;
		}

		SetHPageZeroBusy(page);
		/*
		 * Incrementing this here is a bit of a fib, since
		 * the page hasn't been cleared yet (it will be done
		 * immediately after dropping the lock below). But
		 * it keeps the count consistent with the overall
		 * free count in case the page gets taken off the
		 * freelist while we're working on it.
		 */
		h->bc.free_huge_pages_zero_node[nid]++;
		h->bc.free_huge_pages_zero++;
		spin_unlock_irq(&hugetlb_lock);

		/*
		 * HWPoison pages may show up on the freelist.
		 * Don't try to zero it out, but do set the flag
		 * and counts, so that we don't consider it again.
		 */
		if (!PageHWPoison(page))
			hugetlb_clear_huge_page(page, 0, pages_per_huge_page(h));

		spin_lock_irq(&hugetlb_lock);
		SetHPagePreZeroed(page);
		ClearHPageZeroBusy(page);

		/*
		 * If the page is still on the free list, move
		 * it to the head.
		 */
		if (HPageFreed(page))
			list_move(&page->lru, freelist);

		/*
		 * If someone was waiting for the zero to
		 * finish, wake them up.
		 */
		if (waitqueue_active(&h->bc.dqzero_wait[nid]))
			wake_up(&h->bc.dqzero_wait[nid]);
		spin_unlock_irq(&hugetlb_lock);
	}

	mutex_lock(&prezero_chg_lock);
	h->bc.hzerod[nid] = NULL;
	mutex_unlock(&prezero_chg_lock);

	kfree(ki);
	pr_info("kthread %s exit\n", current->comm);
	return 0;
}

void khzerod_wakeup_node(struct hstate *h, int nid)
{
	const struct cpumask *cpumask = cpumask_of_node(nid);

	if (h->bc.hzerod[nid] && h->free_huge_pages &&
			!(cpumask_any_and(cpumask, &hugetlb_background_clr_cpumask) >= nr_cpu_ids))
		wake_up(&h->bc.hzerod_wait[nid]);
}

static void khzerod_run_hstate_nid(struct hstate *h, int nid)
{
	struct khzerod_info *ki;
	struct task_struct *t;

	mutex_lock(&prezero_chg_lock);

	if (h->bc.hzerod[nid] != NULL) {
		khzerod_wakeup_node(h, nid);
		goto out;
	}

	ki = kmalloc(sizeof(*ki), GFP_KERNEL);
	if (ki == NULL)
		goto out;

	ki->h = h;
	ki->nid = nid;
	t = kthread_run(khzerod, ki, "khzerod-%luM-%d", huge_page_size(h) / 1024 / 1024, nid);
	if (IS_ERR(t)) {
		kfree(ki);
		pr_err("could not run khzerod on node %d for size %lukB\n",
		       nid, huge_page_size(h) / 1024);
	} else {
		h->bc.hzerod[nid] = t;
	}

out:
	mutex_unlock(&prezero_chg_lock);
}

void khzerod_run(int nid)
{
	struct hstate *h;

	for_each_hstate(h)
		khzerod_run_hstate_nid(h, nid);
}

static void khzerod_stop_hstate_nid(struct hstate *h, int nid)
{
	mutex_lock(&prezero_chg_lock);

	if (h->bc.hzerod[nid] != NULL) {
		kthread_stop(h->bc.hzerod[nid]);
		h->bc.hzerod[nid] = NULL;
	}

	mutex_unlock(&prezero_chg_lock);
}

void khzerod_stop(int nid)
{
	struct hstate *h;

	for_each_hstate(h)
		khzerod_stop_hstate_nid(h, nid);
}
