// SPDX-License-Identifier: GPL-2.0-only
/*
 * guestmem_hugetlb is an allocator for guest_memfd. guest_memfd wraps HugeTLB
 * as an allocator for guest_memfd.
 */

#include <linux/mm_types.h>
#include <linux/guestmem.h>
#include <linux/hugetlb.h>
#include <linux/hugetlb_cgroup.h>
#include <linux/mempolicy.h>
#include <linux/mm.h>
#include <linux/pagemap.h>

#include <uapi/linux/guestmem.h>

#include "guestmem_hugetlb.h"

void guestmem_hugetlb_handle_folio_put(struct folio *folio)
{
	WARN_ONCE(1, "A placeholder that shouldn't trigger. Work in progress.");
}

struct guestmem_hugetlb_private {
	struct hstate *h;
	struct hugepage_subpool *spool;
	struct hugetlb_cgroup *h_cg_rsvd;
};

static size_t guestmem_hugetlb_nr_pages_in_folio(void *priv)
{
	struct guestmem_hugetlb_private *private = priv;

	return pages_per_huge_page(private->h);
}

static void *guestmem_hugetlb_setup(size_t size, u64 flags)

{
	struct guestmem_hugetlb_private *private;
	struct hugetlb_cgroup *h_cg_rsvd = NULL;
	struct hugepage_subpool *spool;
	unsigned long nr_pages;
	int page_size_log;
	struct hstate *h;
	long hpages;
	int idx;
	int ret;

	page_size_log = (flags >> GUESTMEM_HUGETLB_FLAG_SHIFT) &
			GUESTMEM_HUGETLB_FLAG_MASK;
	h = hstate_sizelog(page_size_log);
	if (!h)
		return ERR_PTR(-EINVAL);

	/*
	 * Check against h because page_size_log could be 0 to request default
	 * HugeTLB page size.
	 */
	if (!IS_ALIGNED(size, huge_page_size(h)))
		return ERR_PTR(-EINVAL);

	private = kzalloc(sizeof(*private), GFP_KERNEL);
	if (!private)
		return ERR_PTR(-ENOMEM);

	/* Creating a subpool makes reservations, hence charge for them now. */
	idx = hstate_index(h);
	nr_pages = size >> PAGE_SHIFT;
	ret = hugetlb_cgroup_charge_cgroup_rsvd(idx, nr_pages, &h_cg_rsvd);
	if (ret)
		goto err_free;

	hpages = size >> huge_page_shift(h);
	spool = hugepage_new_subpool(h, hpages, hpages, false);
	if (!spool)
		goto err_uncharge;

	private->h = h;
	private->spool = spool;
	private->h_cg_rsvd = h_cg_rsvd;

	return private;

err_uncharge:
	ret = -ENOMEM;
	hugetlb_cgroup_uncharge_cgroup_rsvd(idx, nr_pages, h_cg_rsvd);
err_free:
	kfree(private);
	return ERR_PTR(ret);
}

static void guestmem_hugetlb_teardown(void *priv, size_t inode_size)
{
	struct guestmem_hugetlb_private *private = priv;
	unsigned long nr_pages;
	int idx;

	hugepage_put_subpool(private->spool);

	idx = hstate_index(private->h);
	nr_pages = inode_size >> PAGE_SHIFT;
	hugetlb_cgroup_uncharge_cgroup_rsvd(idx, nr_pages, private->h_cg_rsvd);

	kfree(private);
}

static struct folio *guestmem_hugetlb_alloc_folio(void *priv)
{
	struct guestmem_hugetlb_private *private = priv;
	struct mempolicy *mpol;
	struct folio *folio;
	pgoff_t ilx;
	int ret;

	ret = hugepage_subpool_get_pages(private->spool, 1);
	if (ret == -ENOMEM) {
		return ERR_PTR(-ENOMEM);
	} else if (ret > 0) {
		/* guest_memfd will not use surplus pages. */
		goto err_put_pages;
	}

	/*
	 * TODO: mempolicy would probably have to be stored on the inode, use
	 * task policy for now.
	 */
	mpol = get_task_policy(current);

	/* TODO: ignore interleaving for now. */
	ilx = NO_INTERLEAVE_INDEX;

	/*
	 * charge_cgroup_rsvd is false because we already charged reservations
	 * when creating the subpool for this
	 * guest_memfd. use_existing_reservation is true - we're using a
	 * reservation from the guest_memfd's subpool.
	 */
	folio = hugetlb_alloc_folio(private->h, mpol, ilx, false, true);
	mpol_cond_put(mpol);

	if (IS_ERR_OR_NULL(folio))
		goto err_put_pages;

	/*
	 * Clear restore_reserve here so that when this folio is freed,
	 * free_huge_folio() will always attempt to return the reservation to
	 * the subpool.  guest_memfd, unlike regular hugetlb, has no resv_map,
	 * and hence when freeing, the folio needs to be returned to the
	 * subpool.  guest_memfd does not use surplus hugetlb pages, so in
	 * free_huge_folio(), returning to subpool will always succeed and the
	 * hstate reservation will then get restored.
	 *
	 * hugetlbfs does this in hugetlb_add_to_page_cache().
	 */
	folio_clear_hugetlb_restore_reserve(folio);

	hugetlb_set_folio_subpool(folio, private->spool);

	return folio;

err_put_pages:
	hugepage_subpool_put_pages(private->spool, 1);
	return ERR_PTR(-ENOMEM);
}

const struct guestmem_allocator_operations guestmem_hugetlb_ops = {
	.inode_setup = guestmem_hugetlb_setup,
	.inode_teardown = guestmem_hugetlb_teardown,
	.alloc_folio = guestmem_hugetlb_alloc_folio,
	.nr_pages_in_folio = guestmem_hugetlb_nr_pages_in_folio,
};
EXPORT_SYMBOL_GPL(guestmem_hugetlb_ops);
