/* SPDX-License-Identifier: GPL-2.0 */
#ifndef HUGETLB_BACKGROUND_CLEAN_H
#define HUGETLB_BACKGROUND_CLEAN_H

#ifdef CONFIG_BYTEDANCE_HUGETLB_BACKGROUND_CLEAN

#include <linux/hugetlb.h>

void khzerod_run(int nid);
void khzerod_stop(int nid);
void khzerod_wakeup_node(struct hstate *h, int nid);

static inline void prep_clear_prezero(struct page *page)
{
	ClearHPagePreZeroed(page);
	ClearHPageZeroBusy(page);
}

static inline void __hpage_wait_zerobusy(struct hstate *h, struct page *page)
{
	wait_event_cmd(h->bc.dqzero_wait[page_to_nid(page)],
			!HPageZeroBusy(page),
			spin_unlock_irq(&hugetlb_lock),
			spin_lock_irq(&hugetlb_lock));
}

/*
 * Once a page has been taken off the freelist, the new page owner
 * must wait for the pre-zero kthread to finish if it happens
 * to be working on this page (which should be rare).
 */
static inline void hpage_wait_zerobusy(struct hstate *h, struct page *page)
{
	if (!HPageZeroBusy(page))
		return;

	spin_lock_irq(&hugetlb_lock);
	__hpage_wait_zerobusy(h, page);
	spin_unlock_irq(&hugetlb_lock);
}

static inline void hugetlb_clear_huge_page(struct page *page,
		unsigned long addr_hint, unsigned int pages_per_huge_page)
{
	if (!HPagePreZeroed(page))
		clear_huge_page(page, addr_hint, pages_per_huge_page);
}

static inline void pages_zero_statistic_dec(struct hstate *h, int nid)
{
	h->bc.free_huge_pages_zero_node[nid]--;
	h->bc.free_huge_pages_zero--;
}

#else

static inline void khzerod_run(int nid)
{
}

static inline void khzerod_stop(int nid)
{
}

static inline bool HPagePreZeroed(struct page *page)
{
	return false;
}

static inline bool HPageZeroBusy(struct page *page)
{
	return false;
}

static inline void khzerod_wakeup_node(struct hstate *h, int nid)
{
}

static inline void prep_clear_prezero(struct page *page)
{
}

static inline void hugetlb_clear_huge_page(struct page *page,
		unsigned long addr_hint, unsigned int pages_per_huge_page)
{
	clear_huge_page(page, addr_hint, pages_per_huge_page);
}

static inline void hpage_wait_zerobusy(struct hstate *h, struct page *page)
{
}

static inline void __hpage_wait_zerobusy(struct hstate *h, struct page *page)
{
}

static inline void pages_zero_statistic_dec(struct hstate *h, int nid)
{
}

#endif

#endif
