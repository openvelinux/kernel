// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _ASYNC_FORK_H
#define _ASYNC_FORK_H

#include <linux/mm.h>
#include <linux/task_work.h>

#ifdef CONFIG_BYTEDANCE_ASYNC_FORK
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

#endif /* CONFIG_BYTEDANCE_ASYNC_FORK */
#endif /* _ASYNC_FORK_H */
