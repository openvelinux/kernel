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

#endif /* CONFIG_BYTEDANCE_ASYNC_FORK */
#endif /* _ASYNC_FORK_H */
