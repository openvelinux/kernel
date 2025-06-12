// SPDX-License-Identifier: GPL-2.0
#include <linux/anon_inodes.h>
#include <linux/backing-dev.h>
#include <linux/falloc.h>
#include <linux/fs.h>
#include <linux/kvm_host.h>
#include <linux/mempolicy.h>
#include <linux/maple_tree.h>
#include <linux/pseudo_fs.h>
#include <linux/pagemap.h>
#include <linux/pseudo_fs.h>
#include <linux/memcontrol.h>
#include <linux/pagevec.h>

#include "kvm_mm.h"

static struct vfsmount *kvm_gmem_mnt;

struct kvm_gmem {
	struct kvm *kvm;
	struct xarray bindings;
	struct list_head entry;
};

struct kvm_gmem_inode_info {
	struct shared_policy policy;
	struct inode vfs_inode;
	struct maple_tree shareability;
};

enum shareability {
	SHAREABILITY_GUEST = 1,	/* Only the guest can map (fault) folios in this range. */
	SHAREABILITY_ALL = 2,	/* Both guest and host can fault folios in this range. */
};

static inline struct kvm_gmem_inode_info *KVM_GMEM_I(struct inode *inode)
{
	return container_of(inode, struct kvm_gmem_inode_info, vfs_inode);
}

static struct mempolicy *kvm_gmem_get_pgoff_policy(struct kvm_gmem_inode_info *info,
						   pgoff_t index);

static struct folio *kvm_gmem_get_folio(struct inode *inode, pgoff_t index);
static void kvm_gmem_invalidate_begin(struct kvm_gmem *gmem, pgoff_t start,
				      pgoff_t end);
static void kvm_gmem_invalidate_end(struct kvm_gmem *gmem, pgoff_t start,
				    pgoff_t end);

/**
 * folio_file_pfn - like folio_file_page, but return a pfn.
 * @folio: The folio which contains this index.
 * @index: The index we want to look up.
 *
 * Return: The pfn for this index.
 */
static inline kvm_pfn_t folio_file_pfn(struct folio *folio, pgoff_t index)
{
	return folio_pfn(folio) + (index & (folio_nr_pages(folio) - 1));
}

static int kvm_gmem_shareability_setup(struct kvm_gmem_inode_info *info,
				       loff_t size, u64 flags)
{
	enum shareability m;
	pgoff_t last;

	last = (size >> PAGE_SHIFT) - 1;
	m = flags & GUEST_MEMFD_FLAG_INIT_PRIVATE ? SHAREABILITY_GUEST :
						    SHAREABILITY_ALL;
	return mtree_store_range(&info->shareability, 0, last, xa_mk_value(m),
				 GFP_KERNEL);
}

static enum shareability kvm_gmem_shareability_get(struct inode *inode,
						   pgoff_t index)
{
	struct maple_tree *mt;
	void *entry;

	mt = &KVM_GMEM_I(inode)->shareability;
	entry = mtree_load(mt, index);
	WARN(!entry,
	     "Shareability should always be defined for all indices in inode.");

	return xa_to_value(entry);
}

static struct folio *kvm_gmem_get_shared_folio(struct inode *inode, pgoff_t index)
{
	if (kvm_gmem_shareability_get(inode, index) != SHAREABILITY_ALL)
		return ERR_PTR(-EACCES);

	return kvm_gmem_get_folio(inode, index);
}

/**
 * kvm_gmem_shareability_store() - Sets shareability to @value for range.
 *
 * @mt: the shareability maple tree.
 * @index: the range begins at this index in the inode.
 * @nr_pages: number of PAGE_SIZE pages in this range.
 * @value: the shareability value to set for this range.
 *
 * Unlike mtree_store_range(), this function also merges adjacent ranges that
 * have the same values as an optimization. Assumes that all stores to @mt go
 * through this function, such that adjacent ranges are always merged.
 *
 * Return: 0 on success and negative error otherwise.
 */
static int kvm_gmem_shareability_store(struct maple_tree *mt, pgoff_t index,
				       size_t nr_pages, enum shareability value)
{
	MA_STATE(mas, mt, 0, 0);
	unsigned long start;
	unsigned long last;
	void *entry;
	int ret;

	start = index;
	last = start + nr_pages - 1;

	mas_lock(&mas);

	/* Try extending range. entry is NULL on overflow/wrap-around. */
	mas_set_range(&mas, last + 1, last + 1);
	entry = mas_find(&mas, last + 1);
	if (entry && xa_to_value(entry) == value)
		last = mas.last;

	mas_set_range(&mas, start - 1, start - 1);
	entry = mas_find(&mas, start - 1);
	if (entry && xa_to_value(entry) == value)
		start = mas.index;

	mas_set_range(&mas, start, last);
	ret = mas_store_gfp(&mas, xa_mk_value(value), GFP_KERNEL);

	mas_unlock(&mas);

	return ret;
}

struct conversion_work {
	struct list_head list;
	pgoff_t start;
	size_t nr_pages;
};

static int add_to_work_list(struct list_head *list, pgoff_t start, pgoff_t last)
{
	struct conversion_work *work;

	work = kzalloc(sizeof(*work), GFP_KERNEL);
	if (!work)
		return -ENOMEM;

	work->start = start;
	work->nr_pages = last + 1 - start;

	list_add_tail(&work->list, list);

	return 0;
}

static bool kvm_gmem_has_safe_refcount(struct address_space *mapping, pgoff_t start,
				       size_t nr_pages, pgoff_t *error_index)
{
	const int filemap_get_folios_refcount = 1;
	struct folio_batch fbatch;
	bool refcount_safe;
	pgoff_t last;
	int i;

	last = start + nr_pages - 1;
	refcount_safe = true;

	folio_batch_init(&fbatch);
	while (refcount_safe &&
	       filemap_get_folios(mapping, &start, last, &fbatch)) {

		for (i = 0; i < folio_batch_count(&fbatch); ++i) {
			int filemap_refcount;
			int safe_refcount;
			struct folio *f;

			f = fbatch.folios[i];
			filemap_refcount = folio_nr_pages(f);

			safe_refcount = filemap_refcount + filemap_get_folios_refcount;
			if (folio_ref_count(f) != safe_refcount) {
				refcount_safe = false;
				*error_index = f->index;
				break;
			}
		}

		folio_batch_release(&fbatch);
	}

	return refcount_safe;
}

static int kvm_gmem_shareability_apply(struct inode *inode,
				       struct conversion_work *work,
				       enum shareability m)
{
	struct maple_tree *mt;

	mt = &KVM_GMEM_I(inode)->shareability;

	/*
	 * If a folio has been allocated then it was possibly in a private
	 * state prior to conversion. Ensure arch invalidations are issued
	 * to return the folio to a normal/shared state as defined by the
	 * architecture before tracking it as shared in gmem.
	 */
	if (m == SHAREABILITY_ALL) {
		pgoff_t idx;

		for (idx = work->start; idx < work->start + work->nr_pages; idx++) {
			struct folio *folio = filemap_lock_folio(inode->i_mapping, idx);

			if (!IS_ERR(folio)) {
				kvm_arch_gmem_invalidate(folio_pfn(folio),
							 folio_pfn(folio) + folio_nr_pages(folio));
				folio_unlock(folio);
				folio_put(folio);
			}
		}
	}

	return kvm_gmem_shareability_store(mt, work->start, work->nr_pages, m);
}

static int kvm_gmem_convert_compute_work(struct inode *inode, pgoff_t start,
					 size_t nr_pages, enum shareability m,
					 struct list_head *work_list)
{
	struct maple_tree *mt;
	struct ma_state mas;
	pgoff_t last;
	void *entry;
	int ret;

	last = start + nr_pages - 1;

	mt = &KVM_GMEM_I(inode)->shareability;
	ret = 0;

	mas_init(&mas, mt, start);

	rcu_read_lock();
	mas_for_each(&mas, entry, last) {
		enum shareability current_m;
		pgoff_t m_range_index;
		pgoff_t m_range_last;
		int ret;

		m_range_index = max(mas.index, start);
		m_range_last = min(mas.last, last);

		current_m = xa_to_value(entry);
		if (m == current_m)
			continue;

		mas_pause(&mas);
		rcu_read_unlock();
		/* Caller will clean this up on error. */
		ret = add_to_work_list(work_list, m_range_index, m_range_last);
		rcu_read_lock();
		if (ret)
			break;
	}
	rcu_read_unlock();

	return ret;
}

static void kvm_gmem_convert_invalidate_begin(struct inode *inode,
					      struct conversion_work *work)
{
	struct list_head *gmem_list;
	struct kvm_gmem *gmem;
	pgoff_t end;

	end = work->start + work->nr_pages;

	gmem_list = &inode->i_mapping->private_list;
	list_for_each_entry(gmem, gmem_list, entry)
		kvm_gmem_invalidate_begin(gmem, work->start, end);
}

static void kvm_gmem_convert_invalidate_end(struct inode *inode,
					    struct conversion_work *work)
{
	struct list_head *gmem_list;
	struct kvm_gmem *gmem;
	pgoff_t end;

	end = work->start + work->nr_pages;

	gmem_list = &inode->i_mapping->private_list;
	list_for_each_entry(gmem, gmem_list, entry)
		kvm_gmem_invalidate_end(gmem, work->start, end);
}

static int kvm_gmem_convert_should_proceed(struct inode *inode,
					   struct conversion_work *work,
					   bool to_shared, pgoff_t *error_index)
{
	if (!to_shared) {
		unmap_mapping_pages(inode->i_mapping, work->start,
				    work->nr_pages, false);

		if (!kvm_gmem_has_safe_refcount(inode->i_mapping, work->start,
						work->nr_pages, error_index)) {
			return -EAGAIN;
		}
	}

	return 0;
}

static int kvm_gmem_convert_range(struct file *file, pgoff_t start,
				  size_t nr_pages, bool shared,
				  pgoff_t *error_index)
{
	struct conversion_work *work, *tmp, *rollback_stop_item;
	LIST_HEAD(work_list);
	struct inode *inode;
	enum shareability m;
	int ret;

	inode = file_inode(file);

	filemap_invalidate_lock(inode->i_mapping);

	m = shared ? SHAREABILITY_ALL : SHAREABILITY_GUEST;
	ret = kvm_gmem_convert_compute_work(inode, start, nr_pages, m, &work_list);
	if (ret || list_empty(&work_list))
		goto out;

	list_for_each_entry(work, &work_list, list)
		kvm_gmem_convert_invalidate_begin(inode, work);

	list_for_each_entry(work, &work_list, list) {
		ret = kvm_gmem_convert_should_proceed(inode, work, shared,
						      error_index);
		if (ret)
			goto invalidate_end;
	}

	list_for_each_entry(work, &work_list, list) {
		rollback_stop_item = work;
		ret = kvm_gmem_shareability_apply(inode, work, m);
		if (ret)
			break;
	}

	if (ret) {
		m = shared ? SHAREABILITY_GUEST : SHAREABILITY_ALL;
		list_for_each_entry(work, &work_list, list) {
			if (work == rollback_stop_item)
				break;

			WARN_ON(kvm_gmem_shareability_apply(inode, work, m));
		}
	}

invalidate_end:
	list_for_each_entry(work, &work_list, list)
		kvm_gmem_convert_invalidate_end(inode, work);
out:
	filemap_invalidate_unlock(inode->i_mapping);

	list_for_each_entry_safe(work, tmp, &work_list, list) {
		list_del(&work->list);
		kfree(work);
	}

	return ret;
}

static int kvm_gmem_ioctl_convert_range(struct file *file,
					struct kvm_gmem_convert *param,
					bool shared)
{
	pgoff_t error_index;
	size_t nr_pages;
	pgoff_t start;
	int ret;

	if (param->error_offset)
		return -EINVAL;

	if (param->size == 0)
		return 0;

	if (param->offset + param->size < param->offset ||
	    param->offset > file_inode(file)->i_size ||
	    param->offset + param->size > file_inode(file)->i_size)
		return -EINVAL;

	if (!IS_ALIGNED(param->offset, PAGE_SIZE) ||
	    !IS_ALIGNED(param->size, PAGE_SIZE))
		return -EINVAL;

	start = param->offset >> PAGE_SHIFT;
	nr_pages = param->size >> PAGE_SHIFT;

	ret = kvm_gmem_convert_range(file, start, nr_pages, shared, &error_index);
	if (ret)
		param->error_offset = error_index << PAGE_SHIFT;

	return ret;
}

static int __kvm_gmem_prepare_folio(struct kvm *kvm, struct kvm_memory_slot *slot,
				    pgoff_t index, struct folio *folio)
{
#ifdef CONFIG_HAVE_KVM_ARCH_GMEM_PREPARE
	kvm_pfn_t pfn = folio_file_pfn(folio, index);
	gfn_t gfn = slot->base_gfn + index - slot->gmem.pgoff;
	int rc = kvm_arch_gmem_prepare(kvm, gfn, pfn, folio_order(folio));
	if (rc) {
		pr_warn_ratelimited("gmem: Failed to prepare folio for index %lx GFN %llx PFN %llx error %d.\n",
				    index, gfn, pfn, rc);
		return rc;
	}
#endif

	return 0;
}

/*
 * Process @folio, which contains @gfn, so that the guest can use it.
 * The folio must be locked and the gfn must be contained in @slot.
 * On successful return the guest sees a zero page so as to avoid
 * leaking host data and the up-to-date flag is set.
 */
static int kvm_gmem_prepare_folio(struct kvm *kvm, struct kvm_memory_slot *slot,
				  gfn_t gfn, struct folio *folio)
{
	pgoff_t index;

	/*
	 * Preparing huge folios should always be safe, since it should
	 * be possible to split them later if needed.
	 *
	 * Right now the folio order is always going to be zero, but the
	 * code is ready for huge folios.  The only assumption is that
	 * the base pgoff of memslots is naturally aligned with the
	 * requested page order, ensuring that huge folios can also use
	 * huge page table entries for GPA->HPA mapping.
	 *
	 * The order will be passed when creating the guest_memfd, and
	 * checked when creating memslots.
	 */
	WARN_ON(!IS_ALIGNED(slot->gmem.pgoff, 1 << folio_order(folio)));
	index = gfn - slot->base_gfn + slot->gmem.pgoff;
	index = ALIGN_DOWN(index, 1 << folio_order(folio));

	return __kvm_gmem_prepare_folio(kvm, slot, index, folio);
}

static int __kvm_gmem_filemap_add_folio(struct address_space *mapping,
					struct folio *folio, pgoff_t index)
{
	void *shadow = NULL;
	gfp_t gfp;
	int ret;

	gfp = mapping_gfp_mask(mapping);

	__folio_set_locked(folio);
	ret = __filemap_add_folio(mapping, folio, index, gfp, &shadow);
	__folio_clear_locked(folio);

	return ret;
}

/*
 * Adds a folio to the filemap for guest_memfd. Skips adding the folio to any
 * LRU list.
 */
static int kvm_gmem_filemap_add_folio(struct address_space *mapping,
					     struct folio *folio, pgoff_t index)
{
	int ret;

	ret = __kvm_gmem_filemap_add_folio(mapping, folio, index);
	if (!ret)
		folio_set_unevictable(folio);

	return ret;
}

/*
 * Returns a locked folio on success.  The caller is responsible for
 * setting the up-to-date flag before the memory is mapped into the guest.
 * There is no backing storage for the memory, so the folio will remain
 * up-to-date until it's removed.
 *
 * Ignore accessed, referenced, and dirty flags.  The memory is
 * unevictable and there is no storage to write back to.
 */
static struct folio *kvm_gmem_get_folio(struct inode *inode, pgoff_t index)
{
	struct mempolicy *policy;
	struct folio *folio;
	gfp_t gfp;
	int ret;

repeat:
	folio = filemap_lock_folio(inode->i_mapping, index);
	if (!IS_ERR(folio))
		return folio;

	gfp = mapping_gfp_mask(inode->i_mapping);
	policy = kvm_gmem_get_pgoff_policy(KVM_GMEM_I(inode), index);

	/* TODO: Support huge pages. */
	folio = filemap_alloc_folio(gfp, 0, policy);
	mpol_cond_put(policy);
	if (!folio)
		return ERR_PTR(-ENOMEM);

	ret = mem_cgroup_charge(folio, NULL, gfp);
	if (ret) {
		folio_put(folio);
		return ERR_PTR(ret);
	}

	ret = kvm_gmem_filemap_add_folio(inode->i_mapping, folio, index);
	if (ret) {
		folio_put(folio);

		/*
		 * There was a race, two threads tried to get a folio indexing
		 * to the same location in the filemap. The losing thread should
		 * free the allocated folio, then lock the folio added to the
		 * filemap by the winning thread.
		 */
		if (ret == -EEXIST)
			goto repeat;

		return ERR_PTR(ret);
	}

	__folio_set_locked(folio);
	return folio;
}

static void kvm_gmem_invalidate_begin(struct kvm_gmem *gmem, pgoff_t start,
				      pgoff_t end)
{
	bool flush = false, found_memslot = false;
	struct kvm_memory_slot *slot;
	struct kvm *kvm = gmem->kvm;
	unsigned long index;

	xa_for_each_range(&gmem->bindings, index, slot, start, end - 1) {
		enum kvm_gfn_range_filter filter;
		pgoff_t pgoff = slot->gmem.pgoff;

		filter = KVM_FILTER_PRIVATE;
		if (kvm_memslot_is_gmem_only(slot)) {
			/*
			 * Unmapping would also cause invalidation, but cannot
			 * rely on mmu_notifiers to do invalidation via
			 * unmapping, since memory may not be mapped to
			 * userspace.
			 */
			filter |= KVM_FILTER_SHARED;
		}

		struct kvm_gfn_range gfn_range = {
			.start = slot->base_gfn + max(pgoff, start) - pgoff,
			.end = slot->base_gfn + min(pgoff + slot->npages, end) - pgoff,
			.slot = slot,
			.may_block = true,
			.attr_filter = filter,
		};

		if (!found_memslot) {
			found_memslot = true;

			KVM_MMU_LOCK(kvm);
			kvm_mmu_invalidate_begin(kvm);
		}

		flush |= kvm_mmu_unmap_gfn_range(kvm, &gfn_range);
	}

	if (flush)
		kvm_flush_remote_tlbs(kvm);

	if (found_memslot)
		KVM_MMU_UNLOCK(kvm);
}

static void kvm_gmem_invalidate_end(struct kvm_gmem *gmem, pgoff_t start,
				    pgoff_t end)
{
	struct kvm *kvm = gmem->kvm;

	if (xa_find(&gmem->bindings, &start, end - 1, XA_PRESENT)) {
		KVM_MMU_LOCK(kvm);
		kvm_mmu_invalidate_end(kvm);
		KVM_MMU_UNLOCK(kvm);
	}
}

static long kvm_gmem_punch_hole(struct inode *inode, loff_t offset, loff_t len)
{
	struct list_head *gmem_list = &inode->i_mapping->private_list;
	pgoff_t start = offset >> PAGE_SHIFT;
	pgoff_t end = (offset + len) >> PAGE_SHIFT;
	struct kvm_gmem *gmem;

	/*
	 * Bindings must be stable across invalidation to ensure the start+end
	 * are balanced.
	 */
	filemap_invalidate_lock(inode->i_mapping);

	list_for_each_entry(gmem, gmem_list, entry)
		kvm_gmem_invalidate_begin(gmem, start, end);

	truncate_inode_pages_range(inode->i_mapping, offset, offset + len - 1);

	list_for_each_entry(gmem, gmem_list, entry)
		kvm_gmem_invalidate_end(gmem, start, end);

	filemap_invalidate_unlock(inode->i_mapping);

	return 0;
}

static long kvm_gmem_allocate(struct inode *inode, loff_t offset, loff_t len)
{
	struct address_space *mapping = inode->i_mapping;
	pgoff_t start, index, end;
	int r;

	/* Dedicated guest is immutable by default. */
	if (offset + len > i_size_read(inode))
		return -EINVAL;

	filemap_invalidate_lock_shared(mapping);

	start = offset >> PAGE_SHIFT;
	end = (offset + len) >> PAGE_SHIFT;

	r = 0;
	for (index = start; index < end; ) {
		struct folio *folio;

		if (signal_pending(current)) {
			r = -EINTR;
			break;
		}

		folio = kvm_gmem_get_folio(inode, index);
		if (IS_ERR(folio)) {
			r = PTR_ERR(folio);
			break;
		}

		index = folio_next_index(folio);

		folio_unlock(folio);
		folio_put(folio);

		/* 64-bit only, wrapping the index should be impossible. */
		if (WARN_ON_ONCE(!index))
			break;

		cond_resched();
	}

	filemap_invalidate_unlock_shared(mapping);

	return r;
}

static long kvm_gmem_fallocate(struct file *file, int mode, loff_t offset,
			       loff_t len)
{
	int ret;

	if (!(mode & FALLOC_FL_KEEP_SIZE))
		return -EOPNOTSUPP;

	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE))
		return -EOPNOTSUPP;

	if (!PAGE_ALIGNED(offset) || !PAGE_ALIGNED(len))
		return -EINVAL;

	if (mode & FALLOC_FL_PUNCH_HOLE)
		ret = kvm_gmem_punch_hole(file_inode(file), offset, len);
	else
		ret = kvm_gmem_allocate(file_inode(file), offset, len);

	if (!ret)
		file_modified(file);
	return ret;
}

static int kvm_gmem_release(struct inode *inode, struct file *file)
{
	struct kvm_gmem *gmem = file->private_data;
	struct kvm_memory_slot *slot;
	struct kvm *kvm = gmem->kvm;
	unsigned long index;

	/*
	 * Prevent concurrent attempts to *unbind* a memslot.  This is the last
	 * reference to the file and thus no new bindings can be created, but
	 * dereferencing the slot for existing bindings needs to be protected
	 * against memslot updates, specifically so that unbind doesn't race
	 * and free the memslot (kvm_gmem_get_file() will return NULL).
	 */
	mutex_lock(&kvm->slots_lock);

	filemap_invalidate_lock(inode->i_mapping);

	xa_for_each(&gmem->bindings, index, slot)
		rcu_assign_pointer(slot->gmem.file, NULL);

	synchronize_rcu();

	/*
	 * All in-flight operations are gone and new bindings can be created.
	 * Zap all SPTEs pointed at by this file.  Do not free the backing
	 * memory, as its lifetime is associated with the inode, not the file.
	 */
	kvm_gmem_invalidate_begin(gmem, 0, -1ul);
	kvm_gmem_invalidate_end(gmem, 0, -1ul);

	list_del(&gmem->entry);

	filemap_invalidate_unlock(inode->i_mapping);

	mutex_unlock(&kvm->slots_lock);

	xa_destroy(&gmem->bindings);
	kfree(gmem);

	kvm_put_kvm(kvm);

	return 0;
}

static struct file *kvm_gmem_get_file(struct kvm_memory_slot *slot)
{
	struct file *file;

	rcu_read_lock();

	file = rcu_dereference(slot->gmem.file);
	if (file && !get_file_rcu(file))
		file = NULL;

	rcu_read_unlock();

	return file;
}

static bool kvm_gmem_supports_mmap(struct inode *inode)
{
	const u64 flags = (u64)inode->i_private;

	return flags & GUEST_MEMFD_FLAG_MMAP;
}

static vm_fault_t kvm_gmem_fault_user_mapping(struct vm_fault *vmf)
{
	struct inode *inode = file_inode(vmf->vma->vm_file);
	struct folio *folio;
	vm_fault_t ret = VM_FAULT_LOCKED;

	if (((loff_t)vmf->pgoff << PAGE_SHIFT) >= i_size_read(inode))
		return VM_FAULT_SIGBUS;

	folio = kvm_gmem_get_shared_folio(inode, vmf->pgoff);
	if (IS_ERR(folio)) {
		int err = PTR_ERR(folio);

		if (err == -EAGAIN)
			return VM_FAULT_RETRY;

		return vmf_error(err);
	}

	if (WARN_ON_ONCE(folio_test_large(folio))) {
		ret = VM_FAULT_SIGBUS;
		goto out_folio;
	}

	if (!folio_test_uptodate(folio)) {
		clear_highpage(folio_page(folio, 0));
		folio_mark_uptodate(folio);
	}

	vmf->page = folio_file_page(folio, vmf->pgoff);

out_folio:
	if (ret != VM_FAULT_LOCKED) {
		folio_unlock(folio);
		folio_put(folio);
	}

	return ret;
}

#ifdef CONFIG_NUMA
static int kvm_gmem_set_policy(struct vm_area_struct *vma, struct mempolicy *mpol)
{
	struct inode *inode = file_inode(vma->vm_file);

	return mpol_set_shared_policy(&KVM_GMEM_I(inode)->policy, vma, mpol);
}

static struct mempolicy *kvm_gmem_get_policy(struct vm_area_struct *vma,
					     unsigned long addr, pgoff_t *pgoff)
{
	struct inode *inode = file_inode(vma->vm_file);

	*pgoff = vma->vm_pgoff + ((addr - vma->vm_start) >> PAGE_SHIFT);
	return mpol_shared_policy_lookup(&KVM_GMEM_I(inode)->policy, *pgoff);
}

static struct mempolicy *kvm_gmem_get_pgoff_policy(struct kvm_gmem_inode_info *info,
						   pgoff_t index)
{
	struct mempolicy *mpol;

	mpol = mpol_shared_policy_lookup(&info->policy, index);
	return mpol ? mpol : get_task_policy(current);
}
#else
static struct mempolicy *kvm_gmem_get_pgoff_policy(struct kvm_gmem_inode_info *info,
						   pgoff_t index)
{
	return NULL;
}
#endif /* CONFIG_NUMA */

static const struct vm_operations_struct kvm_gmem_vm_ops = {
	.fault		= kvm_gmem_fault_user_mapping,
#ifdef CONFIG_NUMA
	.get_policy	= kvm_gmem_get_policy,
	.set_policy	= kvm_gmem_set_policy,
#endif
};

static int kvm_gmem_mmap(struct file *file, struct vm_area_struct *vma)
{
	if (!kvm_gmem_supports_mmap(file_inode(file)))
		return -ENODEV;

	if ((vma->vm_flags & (VM_SHARED | VM_MAYSHARE)) !=
	    (VM_SHARED | VM_MAYSHARE)) {
		return -EINVAL;
	}

	vma->vm_ops = &kvm_gmem_vm_ops;

	return 0;
}

static pgoff_t kvm_gmem_get_index(struct kvm_memory_slot *slot, gfn_t gfn)
{
	return gfn - slot->base_gfn + slot->gmem.pgoff;
}

bool kvm_gmem_is_private(struct kvm_memory_slot *slot, gfn_t gfn)
{
	struct inode *inode;
	struct file *file;
	pgoff_t index;
	bool ret;

	file = kvm_gmem_get_file(slot);
	if (!file)
		return false;

	index = kvm_gmem_get_index(slot, gfn);
	inode = file_inode(file);

	filemap_invalidate_lock_shared(inode->i_mapping);
	ret = kvm_gmem_shareability_get(inode, index) == SHAREABILITY_GUEST;
	filemap_invalidate_unlock_shared(inode->i_mapping);

	fput(file);
	return ret;
}
EXPORT_SYMBOL_GPL(kvm_gmem_is_private);

static long kvm_gmem_ioctl(struct file *file, unsigned int ioctl,
			   unsigned long arg)
{
	void __user *argp;
	int r;

	argp = (void __user *)arg;

	switch (ioctl) {
	case KVM_GMEM_CONVERT_SHARED:
	case KVM_GMEM_CONVERT_PRIVATE: {
		struct kvm_gmem_convert param;
		bool to_shared;

		r = -EFAULT;
		if (copy_from_user(&param, argp, sizeof(param)))
			goto out;

		to_shared = ioctl == KVM_GMEM_CONVERT_SHARED;
		r = kvm_gmem_ioctl_convert_range(file, &param, to_shared);
		if (r) {
			if (copy_to_user(argp, &param, sizeof(param))) {
				r = -EFAULT;
				goto out;
			}
		}
		break;
	}

	default:
		r = -ENOTTY;
	}
out:
	return r;
}

static struct file_operations kvm_gmem_fops = {
	.mmap		= kvm_gmem_mmap,
	.open		= generic_file_open,
	.release	= kvm_gmem_release,
	.fallocate	= kvm_gmem_fallocate,
	.unlocked_ioctl	= kvm_gmem_ioctl,
};

static struct kmem_cache *kvm_gmem_inode_cachep;

static struct inode *kvm_gmem_alloc_inode(struct super_block *sb)
{
	struct kvm_gmem_inode_info *info;

	info = alloc_inode_sb(sb, kvm_gmem_inode_cachep, GFP_KERNEL);
	if (!info)
		return NULL;

	mpol_shared_policy_init(&info->policy, NULL);

	return &info->vfs_inode;
}

static void kvm_gmem_free_inode(struct inode *inode)
{
	struct kvm_gmem_inode_info *info = KVM_GMEM_I(inode);

	/*
	 * mtree_destroy() can't be used within rcu callback, hence can't be
	 * done in ->free_inode().
	 */
	mtree_destroy(&info->shareability);

	kmem_cache_free(kvm_gmem_inode_cachep, info);
}

static void kvm_gmem_destroy_inode(struct inode *inode)
{
	mpol_free_shared_policy(&KVM_GMEM_I(inode)->policy);
}

static const struct super_operations kvm_gmem_super_operations = {
	.statfs		= simple_statfs,
	.alloc_inode	= kvm_gmem_alloc_inode,
	.destroy_inode	= kvm_gmem_destroy_inode,
	.free_inode	= kvm_gmem_free_inode,
};

static int kvm_gmem_init_fs_context(struct fs_context *fc)
{
	struct pseudo_fs_context *ctx;

	if (!init_pseudo(fc, GUEST_MEMFD_MAGIC))
		return -ENOMEM;

	fc->s_iflags |= SB_I_NOEXEC;
	fc->s_iflags |= SB_I_NODEV;
	ctx = fc->fs_private;
	ctx->ops = &kvm_gmem_super_operations;

	return 0;
}

static struct file_system_type kvm_gmem_fs = {
	.name		 = "guest_memfd",
	.init_fs_context = kvm_gmem_init_fs_context,
	.kill_sb	 = kill_anon_super,
};

static int kvm_gmem_init_mount(void)
{
	kvm_gmem_mnt = kern_mount(&kvm_gmem_fs);

	if (IS_ERR(kvm_gmem_mnt))
		return PTR_ERR(kvm_gmem_mnt);

	return 0;
}

static void kvm_gmem_init_inode(void *foo)
{
	struct kvm_gmem_inode_info *info = foo;

	inode_init_once(&info->vfs_inode);
}

int kvm_gmem_init(struct module *module)
{
	int ret;
	struct kmem_cache_args args = {
		.align = 0,
		.ctor = kvm_gmem_init_inode,
	};

	kvm_gmem_fops.owner = module;
	kvm_gmem_inode_cachep = kmem_cache_create("kvm_gmem_inode_cache",
						  sizeof(struct kvm_gmem_inode_info),
						  &args, SLAB_ACCOUNT);
	if (!kvm_gmem_inode_cachep)
		return -ENOMEM;
	ret = kvm_gmem_init_mount();
	if (ret) {
		kmem_cache_destroy(kvm_gmem_inode_cachep);
		return ret;
	}
	return 0;
}

void kvm_gmem_exit(void)
{
	kern_unmount(kvm_gmem_mnt);
	kvm_gmem_mnt = NULL;
	rcu_barrier();
	kmem_cache_destroy(kvm_gmem_inode_cachep);
}

static int kvm_gmem_migrate_folio(struct address_space *mapping,
				  struct folio *dst, struct folio *src,
				  enum migrate_mode mode)
{
	WARN_ON_ONCE(1);
	return -EINVAL;
}

static int kvm_gmem_error_page(struct address_space *mapping, struct page *page)
{
	struct list_head *gmem_list = &mapping->private_list;
	struct kvm_gmem *gmem;
	pgoff_t start, end;

	filemap_invalidate_lock_shared(mapping);

	start = page->index;
	end = start + thp_nr_pages(page);

	list_for_each_entry(gmem, gmem_list, entry)
		kvm_gmem_invalidate_begin(gmem, start, end);

	/*
	 * Do not truncate the range, what action is taken in response to the
	 * error is userspace's decision (assuming the architecture supports
	 * gracefully handling memory errors).  If/when the guest attempts to
	 * access a poisoned page, kvm_gmem_get_pfn() will return -EHWPOISON,
	 * at which point KVM can either terminate the VM or propagate the
	 * error to userspace.
	 */

	list_for_each_entry(gmem, gmem_list, entry)
		kvm_gmem_invalidate_end(gmem, start, end);

	filemap_invalidate_unlock_shared(mapping);

	return MF_DELAYED;
}

#ifdef CONFIG_HAVE_KVM_ARCH_GMEM_INVALIDATE
static void kvm_gmem_invalidate(struct folio *folio)
{
	kvm_pfn_t pfn = folio_pfn(folio);

	kvm_arch_gmem_invalidate(pfn, pfn + folio_nr_pages(folio));
}
#else
static inline void kvm_gmem_invalidate(struct folio *folio) {}
#endif

static void kvm_gmem_free_folio(struct folio *folio)
{
	folio_clear_unevictable(folio);

	kvm_gmem_invalidate(folio);
}

static const struct address_space_operations kvm_gmem_aops = {
	.dirty_folio = noop_dirty_folio,
	.migrate_folio	= kvm_gmem_migrate_folio,
	.error_remove_page = kvm_gmem_error_page,
	.free_folio = kvm_gmem_free_folio,
};

static int kvm_gmem_getattr(struct mnt_idmap *idmap, const struct path *path,
			    struct kstat *stat, u32 request_mask,
			    unsigned int query_flags)
{
	struct inode *inode = path->dentry->d_inode;

	generic_fillattr(idmap, request_mask, inode, stat);
	return 0;
}

static int kvm_gmem_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
			    struct iattr *attr)
{
	return -EINVAL;
}
static const struct inode_operations kvm_gmem_iops = {
	.getattr	= kvm_gmem_getattr,
	.setattr	= kvm_gmem_setattr,
};

bool __weak kvm_arch_supports_gmem_mmap(struct kvm *kvm)
{
	return true;
}

static struct inode *kvm_gmem_inode_create(const char *name, loff_t size,
					   u64 flags)
{
	struct kvm_gmem_inode_info *info;
	struct inode *inode;
	int err;

	inode = anon_inode_make_secure_inode(kvm_gmem_mnt->mnt_sb, name, NULL);
	if (IS_ERR(inode))
		return inode;

	err = -ENOMEM;
	info = KVM_GMEM_I(inode);
	if (WARN_ON_ONCE(!info))
		goto out;

	mt_init(&info->shareability);

	err = kvm_gmem_shareability_setup(info, size, flags);
	if (err)
		goto out;

	inode->i_private = (void *)(unsigned long)flags;
	inode->i_op = &kvm_gmem_iops;
	inode->i_mapping->a_ops = &kvm_gmem_aops;
	inode->i_mode |= S_IFREG;
	inode->i_size = size;
	mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
	mapping_set_inaccessible(inode->i_mapping);
	/* Unmovable mappings are supposed to be marked unevictable as well. */
	WARN_ON_ONCE(!mapping_unevictable(inode->i_mapping));

	return inode;

out:
	iput(inode);

	return ERR_PTR(err);
}

static struct file *kvm_gmem_inode_create_getfile(void *priv, loff_t size,
						  u64 flags)
{
	static const char *name = "[kvm-gmem]";
	struct inode *inode;
	struct file *file;
	int err;

	err = -ENOENT;
	/* __fput() will take care of fops_put(). */
	if (!fops_get(&kvm_gmem_fops))
		goto err;

	inode = kvm_gmem_inode_create(name, size, flags);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto err_fops_put;
	}

	file = alloc_file_pseudo(inode, kvm_gmem_mnt, name, O_RDWR,
				 &kvm_gmem_fops);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto err_put_inode;
	}

	file->f_flags |= O_LARGEFILE;
	file->private_data = priv;

	return file;

err_put_inode:
	iput(inode);
err_fops_put:
	fops_put(&kvm_gmem_fops);
err:
	return ERR_PTR(err);
}

static int __kvm_gmem_create(struct kvm *kvm, loff_t size, u64 flags)
{
	struct kvm_gmem *gmem;
	struct file *file;
	int fd, err;

	fd = get_unused_fd_flags(0);
	if (fd < 0)
		return fd;

	gmem = kzalloc(sizeof(*gmem), GFP_KERNEL);
	if (!gmem) {
		err = -ENOMEM;
		goto err_fd;
	}

	file = kvm_gmem_inode_create_getfile(gmem, size, flags);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto err_gmem;
	}

	kvm_get_kvm(kvm);
	gmem->kvm = kvm;
	xa_init(&gmem->bindings);
	list_add(&gmem->entry, &file_inode(file)->i_mapping->private_list);

	fd_install(fd, file);
	return fd;

err_gmem:
	kfree(gmem);
err_fd:
	put_unused_fd(fd);
	return err;
}

int kvm_gmem_create(struct kvm *kvm, struct kvm_create_guest_memfd *args)
{
	loff_t size = args->size;
	u64 flags = args->flags;
	u64 valid_flags = 0;

	if (kvm_arch_supports_gmem_mmap(kvm))
		valid_flags |= GUEST_MEMFD_FLAG_MMAP;

	if (flags & GUEST_MEMFD_FLAG_MMAP)
		valid_flags |= GUEST_MEMFD_FLAG_INIT_PRIVATE;

	if (flags & ~valid_flags)
		return -EINVAL;

	if (size <= 0 || !PAGE_ALIGNED(size))
		return -EINVAL;

	return __kvm_gmem_create(kvm, size, flags);
}

int kvm_gmem_bind(struct kvm *kvm, struct kvm_memory_slot *slot,
		  unsigned int fd, loff_t offset)
{
	loff_t size = slot->npages << PAGE_SHIFT;
	unsigned long start, end;
	struct kvm_gmem *gmem;
	struct inode *inode;
	struct file *file;
	int r = -EINVAL;

	BUILD_BUG_ON(sizeof(gfn_t) != sizeof(slot->gmem.pgoff));

	file = fget(fd);
	if (!file)
		return -EBADF;

	if (file->f_op != &kvm_gmem_fops)
		goto err;

	gmem = file->private_data;
	if (gmem->kvm != kvm)
		goto err;

	inode = file_inode(file);

	if (offset < 0 || !PAGE_ALIGNED(offset) ||
	    offset + size > i_size_read(inode))
		goto err;

	filemap_invalidate_lock(inode->i_mapping);

	start = offset >> PAGE_SHIFT;
	end = start + slot->npages;

	if (!xa_empty(&gmem->bindings) &&
	    xa_find(&gmem->bindings, &start, end - 1, XA_PRESENT)) {
		filemap_invalidate_unlock(inode->i_mapping);
		goto err;
	}

	/*
	 * No synchronize_rcu() needed, any in-flight readers are guaranteed to
	 * be see either a NULL file or this new file, no need for them to go
	 * away.
	 */
	rcu_assign_pointer(slot->gmem.file, file);
	slot->gmem.pgoff = start;
	if (kvm_gmem_supports_mmap(inode))
		slot->flags |= KVM_MEMSLOT_GMEM_ONLY;

	xa_store_range(&gmem->bindings, start, end - 1, slot, GFP_KERNEL);
	filemap_invalidate_unlock(inode->i_mapping);

	/*
	 * Drop the reference to the file, even on success.  The file pins KVM,
	 * not the other way 'round.  Active bindings are invalidated if the
	 * file is closed before memslots are destroyed.
	 */
	r = 0;
err:
	fput(file);
	return r;
}

void kvm_gmem_unbind(struct kvm_memory_slot *slot)
{
	unsigned long start = slot->gmem.pgoff;
	unsigned long end = start + slot->npages;
	struct kvm_gmem *gmem;
	struct file *file;

	/*
	 * Nothing to do if the underlying file was already closed (or is being
	 * closed right now), kvm_gmem_release() invalidates all bindings.
	 */
	file = kvm_gmem_get_file(slot);
	if (!file)
		return;

	gmem = file->private_data;

	filemap_invalidate_lock(file->f_mapping);
	xa_store_range(&gmem->bindings, start, end - 1, NULL, GFP_KERNEL);
	rcu_assign_pointer(slot->gmem.file, NULL);
	synchronize_rcu();
	filemap_invalidate_unlock(file->f_mapping);

	fput(file);
}

/* Returns a locked folio on success.  */
static struct folio *__kvm_gmem_get_pfn(struct file *file,
					struct kvm_memory_slot *slot,
					pgoff_t index, kvm_pfn_t *pfn,
					int *max_order)
{
	struct kvm_gmem *gmem = file->private_data;
	struct folio *folio;

	if (file != slot->gmem.file) {
		WARN_ON_ONCE(slot->gmem.file);
		return ERR_PTR(-EFAULT);
	}

	gmem = file->private_data;
	if (xa_load(&gmem->bindings, index) != slot) {
		WARN_ON_ONCE(xa_load(&gmem->bindings, index));
		return ERR_PTR(-EIO);
	}

	folio = kvm_gmem_get_folio(file_inode(file), index);
	if (IS_ERR(folio))
		return folio;

	if (folio_test_hwpoison(folio)) {
		folio_unlock(folio);
		folio_put(folio);
		return ERR_PTR(-EHWPOISON);
	}

	*pfn = folio_file_pfn(folio, index);
	if (max_order)
		*max_order = 0;


	return folio;
}

int kvm_gmem_get_pfn(struct kvm *kvm, struct kvm_memory_slot *slot,
		     gfn_t gfn, kvm_pfn_t *pfn, int *max_order)
{
	pgoff_t index = kvm_gmem_get_index(slot, gfn);
	struct file *file = kvm_gmem_get_file(slot);
	struct folio *folio;
	int r = 0;

	if (!file)
		return -EFAULT;

	filemap_invalidate_lock_shared(file_inode(file)->i_mapping);

	folio = __kvm_gmem_get_pfn(file, slot, index, pfn, max_order);
	if (IS_ERR(folio)) {
		r = PTR_ERR(folio);
		goto out;
	}

	if (!folio_test_uptodate(folio)) {
		unsigned long i, nr_pages = folio_nr_pages(folio);

		for (i = 0; i < nr_pages; i++)
			clear_highpage(folio_page(folio, i));
		folio_mark_uptodate(folio);
	}

	if (!kvm_memslot_is_gmem_only(slot) ||
	    kvm_gmem_shareability_get(file_inode(file), index) == SHAREABILITY_GUEST)
		r = kvm_gmem_prepare_folio(kvm, slot, gfn, folio);

	folio_unlock(folio);
	if (r < 0)
		folio_put(folio);
out:
	filemap_invalidate_unlock_shared(file_inode(file)->i_mapping);
	fput(file);
	return r;
}
EXPORT_SYMBOL_GPL(kvm_gmem_get_pfn);

#ifdef CONFIG_HAVE_KVM_ARCH_GMEM_POPULATE
long kvm_gmem_populate(struct kvm *kvm, gfn_t start_gfn, void __user *src, long npages,
		       kvm_gmem_populate_cb post_populate, void *opaque)
{
	struct file *file;
	struct kvm_memory_slot *slot;
	void __user *p;

	int ret = 0, max_order;
	long i;

	lockdep_assert_held(&kvm->slots_lock);
	if (npages < 0)
		return -EINVAL;

	slot = gfn_to_memslot(kvm, start_gfn);
	if (!kvm_slot_has_gmem(slot))
		return -EINVAL;

	file = kvm_gmem_get_file(slot);
	if (!file)
		return -EFAULT;

	filemap_invalidate_lock(file->f_mapping);

	npages = min_t(ulong, slot->npages - (start_gfn - slot->base_gfn), npages);
	for (i = 0; i < npages; i += (1 << max_order)) {
		struct folio *folio;
		gfn_t gfn = start_gfn + i;
		pgoff_t index = kvm_gmem_get_index(slot, gfn);
		kvm_pfn_t pfn;

		if (signal_pending(current)) {
			ret = -EINTR;
			break;
		}

		folio = __kvm_gmem_get_pfn(file, slot, index, &pfn, &max_order);
		if (IS_ERR(folio)) {
			ret = PTR_ERR(folio);
			break;
		}

		folio_unlock(folio);
		WARN_ON(!IS_ALIGNED(gfn, 1 << max_order) ||
			(npages - i) < (1 << max_order));

		ret = -EINVAL;
		if (!kvm_memslot_is_gmem_only(slot)) {
			while (!kvm_range_has_memory_attributes(kvm, gfn, gfn + (1 << max_order),
								KVM_MEMORY_ATTRIBUTE_PRIVATE,
								KVM_MEMORY_ATTRIBUTE_PRIVATE)) {
				if (!max_order)
					goto put_folio_and_exit;
				max_order--;
			}
		} else {
			max_order = 0;
		}

		p = src ? src + i * PAGE_SIZE : NULL;
		ret = post_populate(kvm, gfn, pfn, p, max_order, opaque);
		if (!ret)
			folio_mark_uptodate(folio);

put_folio_and_exit:
		folio_put(folio);
		if (ret)
			break;
	}

	filemap_invalidate_unlock(file->f_mapping);

	fput(file);
	return ret && !i ? ret : i;
}
EXPORT_SYMBOL_GPL(kvm_gmem_populate);
#endif
