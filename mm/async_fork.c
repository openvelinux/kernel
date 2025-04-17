// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/async_fork.h>
#include <linux/mmu_notifier.h>
#include <linux/sched/mm.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/sysctl.h>
#include "internal.h"

/*
 * May be 0, 1, or 2, can be modified by sysctl.
 * 0: async-fork is disabled globaly
 * 1: async-fork is controlled by mm->async_copy_enabled which can be modified
      by prctl()
 * 2: async-fork is enabled globaly
 */
int __read_mostly async_fork_enabled = 1;

#ifdef CONFIG_PROC_SYSCTL
static int two = 2;
static struct ctl_table async_fork_table[] = {
	{
		.procname	= "async_fork_enabled",
		.data		= &async_fork_enabled,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= &two,
	},
	{ }
};
static __init int async_fork_sysctls_init(void)
{
	register_sysctl_init("kernel", async_fork_table);
	return 0;
}
late_initcall(async_fork_sysctls_init);
#endif /* CONFIG_PROC_SYSCTL */

static void copy_pte_entire_async(struct vm_area_struct *dst_vma,
		struct vm_area_struct *src_vma, pmd_t *dst_pmd, pmd_t *src_pmd,
		unsigned long addr)
{
	struct mm_struct *dst_mm = dst_vma->vm_mm;
	struct mm_struct *src_mm = src_vma->vm_mm;
	pte_t *orig_src_pte, *orig_dst_pte;
	pte_t *src_pte, *dst_pte;
	spinlock_t *src_ptl, *dst_ptl;
	int ret = 0, err = 0;
	int rss[NR_MM_COUNTERS];
	swp_entry_t entry = (swp_entry_t){0};
	struct page *prealloc = NULL;
	unsigned long end;
	struct page *dst_pte_page;

	/*
	 * When parent process proactively performs copying, the passed addr
	 * may not be aligned to PMD_SIZE. For example, the start address of
	 * madvise(). Here, we enforce alignment to PMD_SIZE.
	 */
	addr &= PMD_MASK;
	end = addr + PMD_SIZE;

	/* Only allocate PTE when no OOM has occurred. */
	err = READ_ONCE(dst_mm->async_copy_err);
	if (likely(!err)) {
		err = pte_alloc(dst_mm, dst_pmd);
		if (unlikely(err))
			WRITE_ONCE(dst_mm->async_copy_err, -ENOMEM);
	}

	/*
	 * Lock order of copying pte:
	 * 1. dst_pte PG_lock
	 * 2. src_pte lock
	 * 3. dst_pte lock
	*/
	if (unlikely(err)) {
		/* take src_pte lock */
		src_pte = pte_offset_map_lock(src_mm, src_pmd, addr, &src_ptl);
		/*
		 * Other CPUs may have successfully allocated dst_pte, so check
		 * again whether there is dst_pte while holding src_pte lock.
		 */
		if (!pmd_none(*dst_pmd)) {
			pte_unmap_unlock(src_pte, src_ptl);
			goto have_dst_pte; /* Need take dst_pte PG_lock */
		}

		/*
		 * When this CPU holds the src_pte lock, it does not matter even
		 * if other CPUs successfully allocate dst_pte after the above
		 * check, because other CPUs cannot take the src_pte lock.
		 */
		if (pmd_test_async_copy_flag(*src_pmd)) {
			set_pmd_at(src_mm, end - PMD_SIZE, src_pmd,
				   pmd_mkwrite(*src_pmd));
			pmd_clear_async_copy_flag(*src_pmd);
		}
		pte_unmap_unlock(src_pte, src_ptl);
		return;
	}
have_dst_pte:
	dst_pte_page = pmd_page(*dst_pmd);
	lock_page(dst_pte_page); /* take dst_pte PG_lock */
again:
	init_rss_vec(rss);
	src_pte = pte_offset_map_lock(src_mm, src_pmd, addr, &src_ptl);
	orig_src_pte = src_pte;
	if (!pmd_test_async_copy_flag(*src_pmd)) {
		pte_unmap_unlock(orig_src_pte, src_ptl);
		goto unlock_pg_lock;
	}
	if (unlikely(err)) {
		pte_unmap_unlock(orig_src_pte, src_ptl);
		goto restore_pmd;
	}

	dst_pte = pte_offset_map(dst_pmd, addr);
	dst_ptl = pte_lockptr(dst_mm, dst_pmd);
	spin_lock_nested(dst_ptl, SINGLE_DEPTH_NESTING);
	orig_dst_pte = dst_pte;
	arch_enter_lazy_mmu_mode();

	do {
		if (pte_none(*src_pte))
			continue;

		if (unlikely(!pte_present(*src_pte))) {
			ret = copy_nonpresent_pte(dst_mm, src_mm,
						  dst_pte, src_pte,
						  dst_vma, src_vma,
						  addr, rss);
			if (ret == -EIO) {
				entry = pte_to_swp_entry(*src_pte);
				break;
			} else if (ret == -EBUSY) {
				break;
			} else if (!ret) {
				continue;
			}

			/*
			 * Device exclusive entry restored, continue by copying
			 * the now present pte.
			 */
			WARN_ON_ONCE(ret != -ENOENT);
		}
		/* copy_present_pte() will clear `*prealloc' if consumed */
		ret = copy_present_pte(dst_vma, src_vma, dst_pte, src_pte,
				       addr, rss, &prealloc);
		/*
		 * If we need a pre-allocated page for this pte, drop the
		 * locks, allocate, and try again.
		 */
		if (unlikely(ret == -EAGAIN))
			break;
		if (unlikely(prealloc)) {
			/*
			 * pre-alloc page cannot be reused by next time so as
			 * to strictly follow mempolicy (e.g., alloc_page_vma()
			 * will allocate page according to address).  This
			 * could only happen if one pinned pte changed.
			 */
			put_page(prealloc);
			prealloc = NULL;
		}
	} while (dst_pte++, src_pte++, addr += PAGE_SIZE, addr != end);

	arch_leave_lazy_mmu_mode();
	add_mm_rss_vec(dst_mm, rss);
	pte_unmap_unlock(orig_dst_pte, dst_ptl);
	pte_unmap_unlock(orig_src_pte, src_ptl);

	if (ret == -EIO) {
		VM_WARN_ON_ONCE(!entry.val);
		if (add_swap_count_continuation(entry, GFP_KERNEL) < 0) {
			ret = -ENOMEM;
			goto out;
		}
		entry.val = 0;
	} else if (ret == -EBUSY) {
		goto out;
	} else if (ret ==  -EAGAIN) {
		prealloc = page_copy_prealloc(src_mm, src_vma, addr);
		if (!prealloc) {
			ret = -ENOMEM;
			goto out;
		}
	} else if (ret) {
		VM_WARN_ON_ONCE(1);
	}

	/* We've captured and resolved the error. Reset, try again. */
	ret = 0;

	if (addr != end)
		goto again;

out:
	if (ret)
		WRITE_ONCE(dst_mm->async_copy_err, ret);
restore_pmd:
	set_pmd_at(src_mm, end - PMD_SIZE, src_pmd, pmd_mkwrite(*src_pmd));
	pmd_clear_async_copy_flag(*src_pmd);
unlock_pg_lock:
	unlock_page(dst_pte_page);
	if (unlikely(prealloc))
		put_page(prealloc);
	cond_resched();
}

static inline void copy_pmd_range_async(struct vm_area_struct *dst_vma,
		struct vm_area_struct *src_vma, pud_t *dst_pud, pud_t *src_pud,
		unsigned long addr, unsigned long end)
{
	pmd_t *src_pmd, *dst_pmd;
	unsigned long next;

	dst_pmd = pmd_offset(dst_pud, addr);
	src_pmd = pmd_offset(src_pud, addr);
	do {
		next = pmd_addr_end(addr, end);
		if (is_swap_pmd(*src_pmd) || pmd_devmap(*src_pmd))
			continue;

		if (pmd_none_or_trans_huge_or_clear_bad(src_pmd))
			continue;
		/*
		 * Only consider PTEs that have been marked. If there is no
		 * marking, it may be a new PTE, so ignore it directly.
		 */
		if (!pmd_test_async_copy_flag(*src_pmd))
			continue;

		/* Same as copy_page_range_async(). Ignore OOM and continue. */
		copy_pte_entire_async(dst_vma, src_vma, dst_pmd, src_pmd, addr);
	} while (dst_pmd++, src_pmd++, addr = next, addr != end);
}

static inline void copy_pud_range_async(struct vm_area_struct *dst_vma,
		struct vm_area_struct *src_vma, p4d_t *dst_p4d, p4d_t *src_p4d,
		unsigned long addr, unsigned long end)
{
	pud_t *src_pud, *dst_pud;
	unsigned long next;

	dst_pud = pud_offset(dst_p4d, addr);
	src_pud = pud_offset(src_p4d, addr);
	do {
		next = pud_addr_end(addr, end);
		if (pud_trans_huge(*src_pud) || pud_devmap(*src_pud))
			continue;

		if (pud_none_or_clear_bad(src_pud))
			continue;

		if (pud_none_or_clear_bad(dst_pud))
			continue;
		/* Same as copy_page_range_async(). Ignore OOM and continue. */
		copy_pmd_range_async(dst_vma, src_vma, dst_pud, src_pud, addr,
				     next);
	} while (dst_pud++, src_pud++, addr = next, addr != end);
}

static inline void copy_p4d_range_async(struct vm_area_struct *dst_vma,
		struct vm_area_struct *src_vma, pgd_t *dst_pgd, pgd_t *src_pgd,
		unsigned long addr, unsigned long end)
{
	p4d_t *src_p4d, *dst_p4d;
	unsigned long next;

	dst_p4d = p4d_offset(dst_pgd, addr);
	src_p4d = p4d_offset(src_pgd, addr);
	do {
		next = p4d_addr_end(addr, end);
		if (p4d_none_or_clear_bad(src_p4d))
			continue;

		if (p4d_none_or_clear_bad(dst_p4d))
			continue;
		/* Same as copy_page_range_async(). Ignore OOM and continue. */
		copy_pud_range_async(dst_vma, src_vma, dst_p4d, src_p4d, addr,
				     next);
	} while (dst_p4d++, src_p4d++, addr = next, addr != end);
}

void copy_page_range_async(struct vm_area_struct *dst_vma,
		struct vm_area_struct *src_vma, unsigned long addr,
		unsigned long end)
{
	pgd_t *src_pgd, *dst_pgd;
	unsigned long next;
	struct mm_struct *dst_mm = dst_vma->vm_mm;
	struct mm_struct *src_mm = src_vma->vm_mm;
	bool is_full = false;

	BUG_ON(addr < src_vma->vm_start || end > src_vma->vm_end);
	if (addr == src_vma->vm_start && end == src_vma->vm_end)
		is_full = true;

	/* We never copy a mm_struct which has notifiers asynchronously. */
	BUG_ON(mm_has_notifiers(src_mm));

	dst_pgd = pgd_offset(dst_mm, addr);
	src_pgd = pgd_offset(src_mm, addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none_or_clear_bad(src_pgd))
			continue;

		if (pgd_none_or_clear_bad(dst_pgd))
			continue;
		/*
		 * Unlike copy_page_range(), we need to continue even if OOM
		 * occurs because we need to restore write permissions to PMD
		 * entries.
		 */
		copy_p4d_range_async(dst_vma, src_vma, dst_pgd, src_pgd, addr,
				     next);
	} while (dst_pgd++, src_pgd++, addr = next, addr != end);

	if (is_full)
		WRITE_ONCE(src_vma->child_vma, NULL);
}

static inline void async_copy_finish(struct mm_struct *parent_mm,
				     struct mm_struct *child_mm)
{
	WRITE_ONCE(parent_mm->async_copy_child_mm, NULL);
	WRITE_ONCE(child_mm->async_copy_parent_mm, NULL);

	/* Let lockdep record that we hold a read lock and then release it. */
	rwsem_acquire_read(&parent_mm->mmap_lock.dep_map, 0, 0, _RET_IP_);
	mmap_read_unlock(parent_mm);
	/*
	 * We only acquire the mmap_sem semaphore in read mode, which means the
	 * parent process can also hold this semaphore in read mode. If the
	 * parent process has already checked a VMA or a PMD entry that needs to
	 * be copied, it will try to access the PMD entry of the child's mm and
	 * attempt to copy the page table. However, during this time, if the
	 * child has finished copying all the page tables and has exited the
	 * asynchronous copy state, the child's mm may have changed. As a result,
	 * errors can occur when the parent process tries to access the PMD
	 * entry of the child's mm.
	 *
	 * To prevent this error, we acquire the mmap_sem in write mode and
	 * release it, ensuring that all readers have exited.
	 */
	mmap_write_lock(parent_mm);
	mmap_write_unlock(parent_mm);

#ifdef CONFIG_DEBUG_RWSEMS
	/*
	 * Change the owner of the lock, otherwise releasing it will case a
	 * warning.
	 */
	atomic_long_set(&child_mm->mmap_lock.owner, (long)current);
#endif
	/* Let lockdep record that we hold a write lock and then release it. */
	rwsem_acquire(&child_mm->mmap_lock.dep_map, 0, 0, _RET_IP_);
	mmap_write_unlock(child_mm);
	mmput(parent_mm);
}

void async_copy_fn(struct callback_head *work)
{
	struct mm_struct *child_mm, *parent_mm;
	struct vm_area_struct *vma, *child_vma;

	child_mm = container_of(work, struct mm_struct, async_copy_work);
	parent_mm = child_mm->async_copy_parent_mm;

	/*
	 * This is in the context of child process. Hold mmap_sem of parent
	 * in read mode and mmap_sem of child in write mode at this point.
	 */
	for (vma = parent_mm->mmap; vma; vma = vma->vm_next) {
		/*
		 * vma->child_vma may be cleared concurrently by parent. If so,
		 * means parent have copied the page tables of this VMA.
		 * We allow parent process and child process to
		 * simultaneously copy a VMA's page table because there is a PTE
		 * lock at the underlying level for mutual exclusion.
		 */
		child_vma = READ_ONCE(vma->child_vma);
		if (child_vma)
			copy_page_range_async(child_vma, vma,
					      vma->vm_start, vma->vm_end);
	}
	async_copy_finish(parent_mm, child_mm);

	if (child_mm->async_copy_err) {
		child_mm->async_copy_err = 0;
		/*
		 * Fail to copy the page table and exit the child process itself.
		 * Trick users into receiving a KILL signal so that they treat
		 * it as if they have received a KILL signal.
		 */
		do_exit(SIGKILL);
	}
	/*
	 * Defer the put_user() in schedule_tail() to this point. The copying
	 * of page table has been completed at this time, so it is allowed to
	 * trigger page fault.
	 */
	if (current->set_child_tid)
		put_user(task_pid_vnr(current), current->set_child_tid);
}

int async_fork_rollback(struct mm_struct *parent_mm)
{
	struct mm_struct *child_mm;
	struct vm_area_struct *vma, *child_vma;
	int rc;

	child_mm = parent_mm->async_copy_child_mm;

	for (vma = parent_mm->mmap; vma; vma = vma->vm_next) {
		child_vma = READ_ONCE(vma->child_vma);
		if (child_vma)
			copy_page_range_async(child_vma, vma,
					      vma->vm_start, vma->vm_end);
	}

	async_copy_finish(parent_mm, child_mm);
	rc = child_mm->async_copy_err;
	if (child_mm->async_copy_err)
		child_mm->async_copy_err = 0;
	return rc;
}

static inline pmd_t *get_pmd(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgd = pgd_offset(mm, addr);
	p4d_t *p4d = p4d_offset(pgd, addr);
	pud_t *pud = pud_offset(p4d, addr);
	pmd_t *pmd = pmd_offset(pud, addr);

	return pmd;
}

/* called with src_mm's mmap_sem held in read mode */
void __try_copy_pte_entire_async(struct vm_area_struct *vma,
				 pmd_t *src_pmd, unsigned long addr)
{
	struct mm_struct *dst_mm;
	pmd_t *dst_pmd;
	struct vm_area_struct *child_vma;

	/* vma->child_vma may be cleard concurrently */
	child_vma = READ_ONCE(vma->child_vma);
	if (!child_vma)
		return;

	dst_mm = child_vma->vm_mm;
	dst_pmd = get_pmd(dst_mm, addr);
	copy_pte_entire_async(child_vma, vma, dst_pmd, src_pmd, addr);
}

static inline void clean_pmd_range(struct mm_struct *mm, pud_t *pud,
		struct vm_area_struct *vma, unsigned long addr,
		unsigned long end)
{
	pmd_t *pmd;
	unsigned long next;
	pte_t *pte;
	spinlock_t *ptl;

	pmd = pmd_offset(pud, addr);
	do {
		next = pmd_addr_end(addr, end);
		if (is_swap_pmd(*pmd) || pmd_devmap(*pmd))
			continue;

		if (pmd_none_or_trans_huge_or_clear_bad(pmd))
			continue;

		if (!pmd_test_async_copy_flag(*pmd))
			continue;

		/*
		 * In an extreme case, there may be a race here. If the parent
		 * process is a multi-threaded process, when a thread fork fails
		 * to clean up, other threads may actively copy the page table,
		 * so pte_lock is used for synchronization.
		 */
		pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
		/* check async copy flag again */
		if (pmd_test_async_copy_flag(*pmd)) {
			/*
			 * There should be no race when modifying the pmd entry
			 * in the 5.15 kernel. Race will exist in higher kernel
			 * versions and requires holding the pmd lock.
			 */
			set_pmd_at(mm, addr, pmd, pmd_mkwrite(*pmd));
			pmd_clear_async_copy_flag(*pmd);
		}
		pte_unmap_unlock(pte, ptl);
	} while (pmd++, addr = next, addr != end);
}

static inline void clean_pud_range(struct mm_struct *mm, p4d_t *p4d,
		struct vm_area_struct *vma, unsigned long addr,
		unsigned long end)
{
	pud_t *pud;
	unsigned long next;

	pud = pud_offset(p4d, addr);
	do {
		next = pud_addr_end(addr, end);
		if (pud_trans_huge(*pud) || pud_devmap(*pud))
			continue;

		if (pud_none_or_clear_bad(pud))
			continue;
		clean_pmd_range(mm, pud, vma, addr, next);
	} while (pud++, addr = next, addr != end);
}

static inline void clean_p4d_range(struct mm_struct *mm,
		struct vm_area_struct *vma, pgd_t *pgd, unsigned long addr,
		unsigned long end)
{
	p4d_t *p4d;
	unsigned long next;

	p4d = p4d_offset(pgd, addr);
	do {
		next = p4d_addr_end(addr, end);
		if (p4d_none_or_clear_bad(p4d))
			continue;
		clean_pud_range(mm, p4d, vma, addr, next);
	} while (p4d++, addr = next, addr != end);
}

static inline void clean_page_range(struct mm_struct *mm,
		struct vm_area_struct *vma, unsigned long addr,
		unsigned long end)
{
	pgd_t *pgd;
	unsigned long next;

	BUG_ON(mm_has_notifiers(mm));

	pgd = pgd_offset(mm, addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none_or_clear_bad(pgd))
			continue;
		clean_p4d_range(mm, vma, pgd, addr, next);
	} while (pgd++, addr = next, addr != end);

	WRITE_ONCE(vma->child_vma, NULL);
}

void clean_async_copy(struct mm_struct *child_mm)
{
	struct vm_area_struct *vma;
	struct mm_struct *parent_mm;

	/*
	 * This is in the context of the parent process. Only when fork() fails
	 * and the child process has never run, the parent process may invoke
	 * this cleanup function to roll back fork(). Hold mmap_sem of parent
	 * in read mode and mmap_sem of child in write mode at this point.
	 */
	parent_mm = child_mm->async_copy_parent_mm;

	for (vma = parent_mm->mmap; vma; vma = vma->vm_next) {
		if (READ_ONCE(vma->child_vma)) {
			clean_page_range(parent_mm, vma, vma->vm_start,
					 vma->vm_end);
		}
	}
	async_copy_finish(parent_mm, child_mm);
}
