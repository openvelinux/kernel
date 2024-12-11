// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _ASYNC_FORK_H
#define _ASYNC_FORK_H

#include <linux/mm.h>
#include <linux/task_work.h>
#include <linux/sched/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/memcontrol.h>

#ifdef CONFIG_BYTEDANCE_ASYNC_FORK

extern int async_fork_enabled;

static inline bool pmd_test_async_copy_flag(pmd_t pmd)
{
	struct page *page = pmd_page(pmd);

	return PageAsyncCopy(page);
}

static inline void pmd_set_async_copy_flag(pmd_t pmd)
{
	struct page *page = pmd_page(pmd);

	SetPageAsyncCopy(page);
}

static inline void pmd_clear_async_copy_flag(pmd_t pmd)
{
	struct page *page = pmd_page(pmd);

	ClearPageAsyncCopy(page);
}

void async_copy_fn(struct callback_head *work);

static inline void mm_init_async_copy(struct mm_struct *mm)
{
	mm->async_copy_child_mm = NULL;
	mm->async_copy_parent_mm = NULL;
	init_task_work(&mm->async_copy_work, async_copy_fn);
	mm->async_copy_enabled = false;
}

static inline bool is_parent_mm_in_async_copy(struct mm_struct *parent_mm)
{
	return !!parent_mm->async_copy_child_mm;
}

static inline bool is_child_mm_in_async_copy(struct mm_struct *child_mm)
{
	return !!child_mm->async_copy_parent_mm;
}

static inline bool is_async_copy_target_vma(struct vm_area_struct *vma)
{
	unsigned long flags = VM_HUGETLB | VM_PFNMAP | VM_MIXEDMAP |
			      VM_GROWSDOWN | VM_GROWSUP | VM_MERGEABLE;

	return vma_is_anonymous(vma) && !(vma->vm_flags & flags);
}

static inline bool is_pte_covered_by_only_one_vma(unsigned long addr,
						  unsigned long next)
{
	return (addr & PMD_MASK) == addr && next - addr == PMD_SIZE;
}

static inline bool try_async_copy_pte(struct vm_area_struct *src_vma,
		struct vm_area_struct *dst_vma, pmd_t *src_pmd,
		unsigned long addr, unsigned long next)
{
	struct mm_struct *src_mm = src_vma->vm_mm;
	pmd_t pmd;

	if (!is_parent_mm_in_async_copy(src_mm))
		return false;

	if (!is_async_copy_target_vma(src_vma))
		return false;

	if (!is_pte_covered_by_only_one_vma(addr, next))
		return false;

	pmd = *src_pmd;
	BUG_ON(pmd_test_async_copy_flag(pmd));
	src_vma->child_vma = dst_vma;
	pmd_set_async_copy_flag(pmd);
	pmdp_set_wrprotect(src_mm, addr, src_pmd);

	return true;
}

void __try_copy_pte_entire_async(struct vm_area_struct *vma,
				 pmd_t *src_pmd, unsigned long addr);

static inline void try_copy_pte_entire_async(struct vm_area_struct *vma,
			pmd_t *src_pmd, unsigned long addr)
{
	if (likely(!pmd_test_async_copy_flag(*src_pmd)))
		return;

	__try_copy_pte_entire_async(vma, src_pmd, addr);
}

void copy_page_range_async(struct vm_area_struct *dst_vma,
		struct vm_area_struct *src_vma, unsigned long addr,
		unsigned long end);

static inline void try_copy_page_range_async(struct vm_area_struct *vma,
			unsigned long start, unsigned long end)
{
	struct vm_area_struct *child_vma;

	/* vma->child_vma may be cleard concurrently */
	child_vma = READ_ONCE(vma->child_vma);
	if (likely(!child_vma))
		return;

	copy_page_range_async(child_vma, vma, start, end);
}

void clean_async_copy(struct mm_struct *child_mm);

static inline void try_clean_async_copy(struct mm_struct *mm)
{
	if (likely(!is_child_mm_in_async_copy(mm)))
		return;

	clean_async_copy(mm);
}

static inline bool mm_has_userfaultfd(struct mm_struct *mm)
{
	struct vm_area_struct *vma;

	for (vma = mm->mmap; vma; vma = vma->vm_next)
		if (unlikely(vma->vm_userfaultfd_ctx.ctx))
			return true;
	return false;
}

static inline bool mem_cgroup_async_fork_enabled(struct task_struct *p)
{
	struct mem_cgroup *memcg;
	int enabled = false;

	if (mem_cgroup_disabled())
		return false;
	rcu_read_lock();
	memcg = mem_cgroup_from_task(p);
	if (memcg && READ_ONCE(memcg->async_fork_enabled))
		enabled = true;
	rcu_read_unlock();

	return enabled;
}

/*
 * Called with parent_mm's mmap_sem held in write mode to prevent concurrent
 * usage of async-fork.
 */
static inline void try_enable_async_copy(struct mm_struct *parent_mm,
					 struct mm_struct *child_mm,
					 struct task_struct *p)
{
	int global_enabled;

	global_enabled = READ_ONCE(async_fork_enabled);
	/*
	 * Global variable async_fork_enabled may be 0, 1 or 2. It's default
	 * value is 1. sysctl can control it.
	 *
	 * case 0: disable async-fork
	 * case 1: depend on mm->async_copy_enabled(prctl() can control it) and
		   memcg->async_fork_enabled.
	 * case 2: enable async-fork
	 */
	if (global_enabled == 1) {
		if (READ_ONCE(parent_mm->async_copy_enabled))
			goto pass_check;
		if (mem_cgroup_async_fork_enabled(p))
			goto pass_check;
	} else if (global_enabled == 2)
		goto pass_check;

	return;
pass_check:
	/* Never enable async-fork if this mm has notifiers */
	if (mm_has_notifiers(parent_mm))
		return;

	/*
	 * dup_userfaultfd_complete() may take mmap_sem so disable async-fork
	 * if this mm has userfaultfd.
	 */
	if (mm_has_userfaultfd(parent_mm))
		return;

	parent_mm->async_copy_child_mm = child_mm;
	child_mm->async_copy_parent_mm = parent_mm;
	mmget(parent_mm);
}

#else
static inline void mm_init_async_copy(struct mm_struct *mm) {}
static inline bool is_parent_mm_in_async_copy(struct mm_struct *parent_mm)
{
	return false;
}

static inline bool is_child_mm_in_async_copy(struct mm_struct *child_mm)
{
	return false;
}

static inline bool try_async_copy_pte(struct vm_area_struct *src_vma,
		struct vm_area_struct *dst_vma, pmd_t *src_pmd,
		unsigned long addr, unsigned long next)
{
	return false;
}

static inline void try_copy_pte_entire_async(struct vm_area_struct *vma,
			pmd_t *src_pmd, unsigned long addr) {}

static inline void try_copy_page_range_async(struct vm_area_struct *vma,
			unsigned long start, unsigned long end) {}

static inline void try_clean_async_copy(struct mm_struct *mm) {}

static inline void try_enable_async_copy(struct mm_struct *parent_mm,
					 struct mm_struct *child_mm,
					 struct task_struct *p) {}

#endif /* CONFIG_BYTEDANCE_ASYNC_FORK */
#endif /* _ASYNC_FORK_H */
