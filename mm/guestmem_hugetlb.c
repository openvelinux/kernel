// SPDX-License-Identifier: GPL-2.0-only
/*
 * guestmem_hugetlb is an allocator for guest_memfd. guest_memfd wraps HugeTLB
 * as an allocator for guest_memfd.
 */

#include <linux/guestmem.h>
#include <linux/hugetlb.h>
#include <linux/hugetlb_cgroup.h>
#include <linux/mempolicy.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/pagemap.h>
#include <linux/xarray.h>

#include <uapi/linux/guestmem.h>

#include "guestmem_hugetlb.h"
#include "hugetlb_vmemmap.h"

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

static DEFINE_XARRAY(guestmem_hugetlb_stash);

struct guestmem_hugetlb_metadata {
	void *_hugetlb_subpool;
	void *_hugetlb_cgroup;
	void *_hugetlb_hwpoison;
	void *private;
};

struct guestmem_hugetlb_stash_item {
	struct guestmem_hugetlb_metadata hugetlb_metadata;
	/* hstate tracks the original size of this folio. */
	struct hstate *h;
	/* Count of split pages, individually freed, waiting to be merged. */
	atomic_t nr_pages_waiting_to_be_merged;
	int split_count;
};

struct workqueue_struct *guestmem_hugetlb_wq __ro_after_init;
static struct work_struct guestmem_hugetlb_cleanup_work;
static LLIST_HEAD(guestmem_hugetlb_cleanup_list);

static inline void guestmem_hugetlb_register_folio_put_callback(struct folio *folio)
{
	__folio_set_guestmem_hugetlb(folio);
}

static inline void guestmem_hugetlb_unregister_folio_put_callback(struct folio *folio)
{
	__folio_clear_guestmem_hugetlb(folio);
}

static inline void guestmem_hugetlb_defer_cleanup(struct folio *folio)
{
	struct llist_node *node;

	/*
	 * Reuse the folio->mapping pointer as a struct llist_node, since
	 * folio->mapping is NULL at this point.
	 */
	BUILD_BUG_ON(sizeof(folio->mapping) != sizeof(struct llist_node));
	node = (struct llist_node *)&folio->mapping;

	/*
	 * Only schedule work if list is previously empty. Otherwise,
	 * schedule_work() had been called but the workfn hasn't retrieved the
	 * list yet.
	 */
	if (llist_add(node, &guestmem_hugetlb_cleanup_list))
		queue_work(guestmem_hugetlb_wq, &guestmem_hugetlb_cleanup_work);
}

void guestmem_hugetlb_handle_folio_put(struct folio *folio)
{
	guestmem_hugetlb_unregister_folio_put_callback(folio);

	/*
	 * folio_put() can be called in interrupt context, hence do the work
	 * outside of interrupt context
	 */
	guestmem_hugetlb_defer_cleanup(folio);
}

/*
 * Stash existing hugetlb metadata. Use this function just before splitting a
 * hugetlb page.
 */
static inline void
__guestmem_hugetlb_stash_metadata(struct guestmem_hugetlb_metadata *metadata,
				  struct folio *folio)
{
	/*
	 * (folio->page + 1) doesn't have to be stashed since those fields are
	 * known on split/reconstruct and will be reinitialized anyway.
	 */

	/*
	 * subpool is created for every guest_memfd inode, but the folios will
	 * outlive the inode, hence we store the subpool here.
	 */
	metadata->_hugetlb_subpool = folio->_hugetlb_subpool;
	/*
	 * _hugetlb_cgroup has to be stored for freeing
	 * later. _hugetlb_cgroup_rsvd does not, since it is NULL for
	 * guest_memfd folios anyway. guest_memfd reservations are handled in
	 * the inode.
	 */
	metadata->_hugetlb_cgroup = folio->_hugetlb_cgroup;
	metadata->_hugetlb_hwpoison = folio->_hugetlb_hwpoison;

	/*
	 * HugeTLB flags are stored in folio->private. stash so that ->private
	 * can be used by core-mm.
	 */
	metadata->private = folio->private;
}

static int guestmem_hugetlb_stash_metadata(struct folio *folio)
{
	XA_STATE(xas, &guestmem_hugetlb_stash, 0);
	struct guestmem_hugetlb_stash_item *stash;
	void *entry;

	entry = xa_load(&guestmem_hugetlb_stash, folio_pfn(folio));
	if (entry) {
		stash = entry;
		stash->split_count++;
	} else {
		stash = kzalloc(sizeof(*stash), 1);
		if (!stash)
			return -ENOMEM;
		pr_debug("%s: allocated stash for folio pfn %lx\n",
			 __func__, folio_pfn(folio));
	}

	stash->h = folio_hstate(folio);
	__guestmem_hugetlb_stash_metadata(&stash->hugetlb_metadata, folio);

	if (!entry) {
		xas_set_order(&xas, folio_pfn(folio), folio_order(folio));

		xas_lock(&xas);
		entry = xas_store(&xas, stash);
		xas_unlock(&xas);

		if (xa_is_err(entry)) {
			kfree(stash);
			return xa_err(entry);
		}
	}

	return 0;
}

static inline void
__guestmem_hugetlb_unstash_metadata(struct guestmem_hugetlb_metadata *metadata,
				    struct folio *folio)
{
	folio->_hugetlb_subpool = metadata->_hugetlb_subpool;
	folio->_hugetlb_cgroup = metadata->_hugetlb_cgroup;
	folio->_hugetlb_cgroup_rsvd = NULL;
	folio->_hugetlb_hwpoison = metadata->_hugetlb_hwpoison;

	folio_change_private(folio, metadata->private);
}

static int guestmem_hugetlb_unstash_free_metadata(struct folio *folio)
{
	struct guestmem_hugetlb_stash_item *stash;
	unsigned long pfn;

	pfn = folio_pfn(folio);

	stash = xa_load(&guestmem_hugetlb_stash, pfn);

	__guestmem_hugetlb_unstash_metadata(&stash->hugetlb_metadata, folio);

	return 0;
}

/**
 * guestmem_hugetlb_split_folio() - Split a HugeTLB @folio to PAGE_SIZE pages.
 *
 * @folio: The folio to be split.
 *
 * Context: Before splitting, the folio must have a refcount of 0. After
 *          splitting, each split folio has a refcount of 0.
 * Return: 0 on success and negative error otherwise.
 */
static int guestmem_hugetlb_split_folio(struct folio *folio)
{
	long orig_nr_pages;
	int ret;
	int i;

	if (folio_size(folio) == PAGE_SIZE)
		return 0;

	orig_nr_pages = folio_nr_pages(folio);

	/*
	 * hugetlb_vmemmap_restore_folio() has to be called ahead of the rest
	 * because it checks and page type. This doesn't actually split the
	 * folio, so the first few struct pages are still intact.
	 */
	ret = hugetlb_vmemmap_restore(folio_hstate(folio), &folio->page);
	if (ret)
		return ret;

	/*
	 * Stash metadata after vmemmap stuff so the outcome of the vmemmap
	 * restoration is stashed.
	 */
	ret = guestmem_hugetlb_stash_metadata(folio);
	if (ret)
		goto err;

	/*
	 * Can clear without lock because this will not race with the folio
	 * being mapped. folio's page type is overlaid with mapcount and so in
	 * other cases it's necessary to take hugetlb_lock to prevent races with
	 * mapcount increasing.
	 */
	__folio_clear_hugetlb(folio);

	/*
	 * Remove the first folio from h->hugepage_activelist since it is no
	 * longer a HugeTLB page. The other split pages should not be on any
	 * lists.
	 */
	hugetlb_folio_list_del(folio);

	/* Actually split page by undoing prep_compound_page() */
	__folio_clear_head(folio);

#ifdef NR_PAGES_IN_LARGE_FOLIO
	/*
	 * Zero out _nr_pages, otherwise this overlaps with memcg_data,
	 * resulting in lookups on false memcg_data.  _nr_pages doesn't have to
	 * be set to 1 because folio_nr_pages() relies on the presence of the
	 * head flag to return 1 for nr_pages.
	 */
	folio->_nr_pages = 0;
#endif

	for (i = 1; i < orig_nr_pages; ++i) {
		struct page *p = folio_page(folio, i);

		/* Copy flags from the first page to split pages. */
		p->flags = folio->flags;

		p->mapping = NULL;
		clear_compound_head(p);
	}

	return 0;

err:
	hugetlb_vmemmap_optimize(folio_hstate(folio), &folio->page);
	return ret;
}

#define SPLIT_COUNT_MAX 10

static bool guestmem_hugetlb_skip_merge_folio(struct folio *first_folio)
{
	struct guestmem_hugetlb_stash_item *stash;

	stash = xa_load(&guestmem_hugetlb_stash, folio_pfn(first_folio));

	if (WARN_ON_ONCE(!stash))
		return false;

	pr_debug("%s: folio %lx has split count %d (max %d), merge should be skipped: %d\n",
		 __func__, folio_pfn(first_folio), stash->split_count,
		 SPLIT_COUNT_MAX, stash->split_count > SPLIT_COUNT_MAX);

	return stash->split_count > SPLIT_COUNT_MAX;
}

/**
 * guestmem_hugetlb_merge_folio() - Merge a HugeTLB folio from the folio
 * beginning @first_folio.
 *
 * @first_folio: the first folio in a contiguous block of folios to be merged.
 *
 * The size of the contiguous block is tracked in guestmem_hugetlb_stash.
 *
 * Context: The first folio is checked to have a refcount of 0 before
 *          reconstruction. After reconstruction, the reconstructed folio has a
 *          refcount of 0.
 */
static void guestmem_hugetlb_merge_folio(struct folio *first_folio)
{
	struct guestmem_hugetlb_stash_item *stash;
	struct hstate *h;

	stash = xa_load(&guestmem_hugetlb_stash, folio_pfn(first_folio));
	h = stash->h;

	/*
	 * This is the step that does the merge. prep_compound_page() will write
	 * to pages 1 and 2 as well, so guestmem_unstash_hugetlb_metadata() has
	 * to come after this.
	 */
	prep_compound_page(&first_folio->page, huge_page_order(h));

	WARN_ON(guestmem_hugetlb_unstash_free_metadata(first_folio));

	/*
	 * prep_compound_page() will set up mapping on tail pages. For
	 * completeness, clear mapping on head page.
	 */
	first_folio->mapping = NULL;

	__folio_set_hugetlb(first_folio);

	hugetlb_folio_list_add(first_folio, &h->hugepage_activelist);

	hugetlb_vmemmap_optimize(h, &first_folio->page);
}

static struct folio *guestmem_hugetlb_maybe_merge_folio(struct folio *folio)
{
	struct guestmem_hugetlb_stash_item *stash;
	unsigned long first_folio_pfn;
	struct folio *first_folio;
	unsigned long pfn;
	size_t nr_pages;

	pfn = folio_pfn(folio);

	stash = xa_load(&guestmem_hugetlb_stash, pfn);
	nr_pages = pages_per_huge_page(stash->h);
	if (atomic_inc_return(&stash->nr_pages_waiting_to_be_merged) < nr_pages)
		return NULL;

	first_folio_pfn = round_down(pfn, nr_pages);
	first_folio = pfn_folio(first_folio_pfn);

	guestmem_hugetlb_merge_folio(first_folio);

	return first_folio;
}

static void guestmem_hugetlb_cleanup_folio(struct folio *folio)
{
	struct folio *merged_folio;

	merged_folio = guestmem_hugetlb_maybe_merge_folio(folio);
	if (merged_folio) {
		struct guestmem_hugetlb_stash_item *stash;

		__folio_put(merged_folio);
		stash = xa_erase(&guestmem_hugetlb_stash, folio_pfn(merged_folio));
		kfree(stash);
		pr_debug("%s: freed stash for folio pfn %lx (merged pfn %lx)\n",
			 __func__, folio_pfn(folio), folio_pfn(merged_folio));
	}
}

static void guestmem_hugetlb_cleanup_workfn(struct work_struct *work)
{
	struct llist_node *node;

	node = llist_del_all(&guestmem_hugetlb_cleanup_list);
	while (node) {
		struct folio *folio;

		folio = container_of((struct address_space **)node,
				     struct folio, mapping);

		node = node->next;
		folio->mapping = NULL;

		guestmem_hugetlb_cleanup_folio(folio);
	}
}

static int __init guestmem_hugetlb_init(void)
{
	INIT_WORK(&guestmem_hugetlb_cleanup_work, guestmem_hugetlb_cleanup_workfn);

	guestmem_hugetlb_wq = alloc_workqueue("guestmem_hugetlb",
					      WQ_MEM_RECLAIM | WQ_UNBOUND, 0);
	if (!guestmem_hugetlb_wq)
		return -ENOMEM;

	return 0;
}
subsys_initcall(guestmem_hugetlb_init);

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
	if (!IS_ALIGNED(size, huge_page_size(h))) {
		pr_debug("%s: Rounding up allocation size from 0x%lx to 0x%lx\n",
			 __func__, size, round_up(size, huge_page_size(h)));
		size = round_up(size, huge_page_size(h));
	}

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
	WARN_ON_ONCE(!hpages);
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

static struct folio *guestmem_hugetlb_alloc_folio(void *priv, struct mempolicy *mpol)
{
	struct guestmem_hugetlb_private *private = priv;
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
	if (!mpol)
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

static void guestmem_hugetlb_free_folio(struct folio *folio)
{
	struct guestmem_hugetlb_stash_item *stash;

	stash = xa_load(&guestmem_hugetlb_stash, folio_pfn(folio));

	if (stash && huge_page_size(stash->h) != folio_size(folio)) {
		guestmem_hugetlb_register_folio_put_callback(folio);
		return;
	}

	/*
	 * put callback will handle final stash cleanup after merge, but
	 * for stuff that's already been merged this is the last chance.
	 */
	xa_erase(&guestmem_hugetlb_stash, folio_pfn(folio));
	kfree(stash);
	pr_debug("%s: freed stash for folio pfn %lx\n",
		 __func__, folio_pfn(folio));
}

const struct guestmem_allocator_operations guestmem_hugetlb_ops = {
	.inode_setup = guestmem_hugetlb_setup,
	.inode_teardown = guestmem_hugetlb_teardown,
	.alloc_folio = guestmem_hugetlb_alloc_folio,
	.split_folio = guestmem_hugetlb_split_folio,
	.merge_folio = guestmem_hugetlb_merge_folio,
	.free_folio = guestmem_hugetlb_free_folio,
	.nr_pages_in_folio = guestmem_hugetlb_nr_pages_in_folio,
	.skip_merge_folio = guestmem_hugetlb_skip_merge_folio,
};
EXPORT_SYMBOL_GPL(guestmem_hugetlb_ops);
