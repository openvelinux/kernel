// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _ASYNC_FORK_H
#define _ASYNC_FORK_H

#include <linux/mm.h>

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

#endif /* CONFIG_BYTEDANCE_ASYNC_FORK */
#endif /* _ASYNC_FORK_H */
