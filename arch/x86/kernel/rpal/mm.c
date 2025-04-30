// SPDX-License-Identifier: GPL-2.0-only
#include <linux/rpal.h>
#include <linux/sched/mm.h>
#include <linux/mman.h>
#include <linux/hugetlb.h>
#include <linux/security.h>
#include <linux/sched/task.h>

#include <asm/pgalloc.h>
#include <asm/tlbflush.h>

#include "internal.h"

void rpal_munmap(struct vm_area_struct *area);
const struct vm_operations_struct rpal_vm_ops = { .close = rpal_munmap };

bool rpal_is_correct_address(struct rpal_service *rs, unsigned long address)
{
	if (likely(rs->base <= address &&
		   address < rs->base + RPAL_ADDR_SPACE_SIZE))
		return true;

	/*
	 * [rs->base, rs->base + RPAL_ADDR_SPACE_SIZE) is always a
	 * sub range of [RPAL_ADDRESS_SPACE_LOW, RPAL_ADDRESS_SPACE_HIGH).
	 * Therefore, we can only check whether the address is in
	 * [RPAL_ADDRESS_SPACE_LOW, RPAL_ADDRESS_SPACE_HIGH) to determine
	 * whether the address may belong to another RPAL service.
	 */
	if (address >= RPAL_ADDRESS_SPACE_LOW &&
	    address < RPAL_ADDRESS_SPACE_HIGH)
		return false;

	return true;
}

static inline int rpal_balloon_mapping(unsigned long base, unsigned long size)
{
	struct vm_area_struct *vma;
	unsigned long addr, populate;
	int is_fail = 0;

	if (size == 0)
		return 0;

	addr = do_mmap(NULL, base, size, PROT_NONE,
		       MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, 0, &populate,
		       NULL);

	is_fail = base != addr;

	if (is_fail) {
		pr_info("rpal: Balloon mapping 0x%016lx - 0x%016lx, %s, addr: 0x%016lx\n",
			base, base + size, is_fail ? "Fail" : "Success", addr);
	}
	vma = find_vma(current->mm, addr);
	if (vma->vm_start != addr || vma->vm_end != addr + size) {
		is_fail = 1;
		rpal_err("rpal: find vma 0x%016lx - 0x%016lx fail\n", addr,
			 addr + size);
	} else {
		vma->vm_flags |= VM_DONTEXPAND | VM_PFNMAP | VM_DONTDUMP;
	}

	return is_fail;
}

#define RPAL_USER_TOP TASK_SIZE

int rpal_balloon_init(unsigned long base)
{
	unsigned long top;
	struct mm_struct *mm = current->mm;
	int ret;

	top = base + RPAL_ADDR_SPACE_SIZE;

	mmap_write_lock(mm);

	if (base > mmap_min_addr) {
		ret = rpal_balloon_mapping(mmap_min_addr, base - mmap_min_addr);
		if (ret)
			goto out;
	}

	ret = rpal_balloon_mapping(top, RPAL_USER_TOP - top);
	if (ret && base > mmap_min_addr)
		do_munmap(mm, mmap_min_addr, base - mmap_min_addr, NULL);

out:
	mmap_write_unlock(mm);

	return ret;
}

/*
 * Since the user address space size of rpal process is 512G, which
 * is the size of one p4d, we assume p4d entry will never change after
 * rpal process is created.
 */
static int mm_link_p4d(struct mm_struct *dst_mm, p4d_t src_p4d,
		       unsigned long addr, int pkey)
{
	spinlock_t *dst_ptl = &dst_mm->page_table_lock;
	unsigned long flags;
	pgd_t *dst_pgdp;
	p4d_t p4d, *dst_p4dp;
	p4dval_t p4dv;
	int ret = 0;

	BUILD_BUG_ON(CONFIG_PGTABLE_LEVELS < 4);

	mmap_write_lock(dst_mm);
	spin_lock_irqsave(dst_ptl, flags);
	dst_pgdp = pgd_offset(dst_mm, addr);
	/*
	 * dst_pgd must exists, otherwise we need to alloc pgd entry. When
	 * src_p4d is freed, we also need to free the pgd entry. This should
	 * be supported in the future.
	 */
	if (unlikely(pgd_none_or_clear_bad(dst_pgdp))) {
		rpal_err("cannot find pgd entry for addr 0x%016lx\n", addr);
		ret = -RPAL_ERR_NOMAPPING;
		goto unlock;
	}

	dst_p4dp = p4d_offset(dst_pgdp, addr);
	if (unlikely(!p4d_none_or_clear_bad(dst_p4dp))) {
		rpal_err("p4d is previously mapped\n");
		ret = -RPAL_ERR_BAD_SERVICE_STATUS;
		goto unlock;
	}

	p4dv = p4d_val(src_p4d);

	p4dv |= _PAGE_RPAL_IGN;

	if (boot_cpu_has(X86_FEATURE_PTI))
		p4d = native_make_p4d((~_PAGE_NX) & p4dv);
	else
		p4d = native_make_p4d(p4dv);

	set_p4d(dst_p4dp, p4d);
	spin_unlock_irqrestore(dst_ptl, flags);
	mmap_write_unlock(dst_mm);

	return 0;
unlock:
	spin_unlock_irqrestore(dst_ptl, flags);
	mmap_write_unlock(dst_mm);
	return ret;
}

static void mm_unlink_p4d(struct mm_struct *mm, unsigned long addr)
{
	spinlock_t *ptl = &mm->page_table_lock;
	unsigned long flags;
	pgd_t *pgdp;
	p4d_t *p4dp;

	mmap_write_lock(mm);
	spin_lock_irqsave(ptl, flags);
	pgdp = pgd_offset(mm, addr);
	p4dp = p4d_offset(pgdp, addr);
	p4d_clear(p4dp);
	spin_unlock_irqrestore(ptl, flags);
	mmap_write_unlock(mm);

	flush_tlb_range(mm->mmap, addr, addr + RPAL_ADDR_SPACE_SIZE);
}

static int get_mm_p4d(struct mm_struct *mm, unsigned long addr, p4d_t *srcp)
{
	spinlock_t *ptl;
	unsigned long flags;
	pgd_t *pgdp;
	p4d_t *p4dp;
	int ret = 0;

	ptl = &mm->page_table_lock;
	spin_lock_irqsave(ptl, flags);
	pgdp = pgd_offset(mm, addr);
	if (pgd_none(*pgdp)) {
		ret = -RPAL_ERR_BAD_SERVICE_STATUS;
		goto out;
	}

	p4dp = p4d_offset(pgdp, addr);
	if (p4d_none(*p4dp) || p4d_bad(*p4dp)) {
		ret = -RPAL_ERR_BAD_SERVICE_STATUS;
		goto out;
	}
	*srcp = *p4dp;

out:
	spin_unlock_irqrestore(ptl, flags);

	return ret;
}

int rpal_map_service(struct rpal_service *tgt)
{
	struct rpal_service *cur = rpal_current_service();
	struct mm_struct *cur_mm, *tgt_mm;
	unsigned long cur_addr, tgt_addr;
	p4d_t cur_p4d, tgt_p4d;
	int pkey = 0;
	int ret = 0;

	cur_mm = current->mm;
	tgt_mm = tgt->mm;
	if (!mmget_not_zero(tgt_mm)) {
		ret = -RPAL_ERR_BAD_SERVICE_STATUS;
		goto out;
	}

	cur_addr = rpal_get_base(cur);
	tgt_addr = rpal_get_base(tgt);

	ret = get_mm_p4d(tgt_mm, tgt_addr, &tgt_p4d);
	if (ret)
		goto put_tgt;

	ret = get_mm_p4d(cur_mm, cur_addr, &cur_p4d);
	if (ret)
		goto put_tgt;

	ret = mm_link_p4d(cur_mm, tgt_p4d, tgt_addr, pkey);
	if (ret)
		goto put_tgt;

	ret = mm_link_p4d(tgt_mm, cur_p4d, cur_addr, pkey);
	if (ret) {
		mm_unlink_p4d(cur_mm, tgt_addr);
		goto put_tgt;
	}

put_tgt:
	mmput(tgt_mm);
out:
	return ret;
}

void rpal_unmap_service(struct rpal_service *tgt)
{
	struct rpal_service *cur = rpal_current_service();
	struct mm_struct *cur_mm, *tgt_mm;
	unsigned long cur_addr, tgt_addr;

	cur_mm = current->mm;
	tgt_mm = tgt->mm;

	cur_addr = rpal_get_base(cur);
	tgt_addr = rpal_get_base(tgt);

	if (mmget_not_zero(tgt_mm)) {
		mm_unlink_p4d(tgt_mm, cur_addr);
		mmput(tgt_mm);
	}
	mm_unlink_p4d(cur_mm, tgt->base);
}

void rpal_munmap(struct vm_area_struct *area)
{
	struct mm_struct *mm = area->vm_mm;
	struct rpal_service *rs = mm->rpal_rs;
	struct rpal_shared_page *rsp = area->vm_private_data;
	unsigned long flags;
	int refcnt = atomic_read(&rsp->refcnt);

	if (mm->rpal_rs == NULL) {
		rpal_err(
			"free shared page after exit_mmap or fork a child process\n");
		return;
	}

	/* TODO: implement a better design of shared memory free */
	if (unlikely(area->vm_start != rsp->user_start ||
		     area->vm_end != rsp->user_end)) {
		rpal_err("free partial of shared pages\n");
		return;
	}

	if (unlikely(refcnt != 0)) {
		rpal_err("refcnt(%d) of shared page is not 0\n", refcnt);
		return;
	}

	spin_lock_irqsave(&rs->lock, flags);
	list_del(&rsp->list);
	spin_unlock_irqrestore(&rs->lock, flags);

	atomic_sub(rsp->npage, &rs->nr_shared_pages);
	__free_pages(rsp->page, get_order(rsp->npage));
	kfree(rsp);
}

int rpal_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct rpal_service *cur = rpal_current_service();
	struct rpal_shared_page *rsp;
	struct page *page = NULL;
	unsigned long size = (unsigned long)(vma->vm_end - vma->vm_start);
	unsigned long flags;
	int nr_pages, npage;
	int order = -1;
	int ret = 0;

	if (!cur) {
		ret = -EINVAL;
		goto out;
	}

	if (!IS_ALIGNED(size, PAGE_SIZE) ||
	    !IS_ALIGNED(vma->vm_start, PAGE_SIZE)) {
		ret = -EINVAL;
		goto out;
	}

	npage = size >> PAGE_SHIFT;
	if (!is_power_of_2(npage)) {
		ret = -EINVAL;
		goto out;
	}

	order = get_order(size);

retry:
	nr_pages = atomic_read(&cur->nr_shared_pages);
	if (nr_pages + npage <= RPAL_MAX_SHARED_PAGES) {
		if (atomic_cmpxchg(&cur->nr_shared_pages, nr_pages,
				   nr_pages + npage) != nr_pages) {
			goto retry;
		}
	} else {
		ret = -ENOMEM;
		goto out;
	}

	rsp = kmalloc(sizeof(*rsp), GFP_KERNEL);
	if (!rsp) {
		ret = -EAGAIN;
		goto dec;
	}

	page = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
	if (!page) {
		ret = -ENOMEM;
		goto free_rsp;
	}

	rsp->user_start = vma->vm_start;
	rsp->user_end = vma->vm_end;
	rsp->kernel_start = (unsigned long)page_address(page);
	rsp->npage = npage;
	rsp->page = page;
	atomic_set(&rsp->refcnt, 0);
	INIT_LIST_HEAD(&rsp->list);

	ret = remap_pfn_range(vma, vma->vm_start, page_to_pfn(page), size,
			      vma->vm_page_prot);
	if (ret)
		goto free_page;

	spin_lock_irqsave(&cur->lock, flags);
	list_add(&rsp->list, &cur->shared_pages);
	spin_unlock_irqrestore(&cur->lock, flags);

	vma->vm_ops = &rpal_vm_ops;
	vma->vm_private_data = rsp;

	return 0;

free_page:
	__free_pages(page, order);
free_rsp:
	kfree(rsp);
dec:
	atomic_sub(npage, &cur->nr_shared_pages);
out:
	return ret;
}

void rpal_exit_mmap(struct mm_struct *mm)
{
	struct rpal_service *rs = mm->rpal_rs;

	if (rs) {
		int nr_pages;

		mm->rpal_rs = NULL;
		nr_pages = atomic_read(&rs->nr_shared_pages);
		if (unlikely(nr_pages != 0))
			rpal_err("shared page is not zero: %d\n", nr_pages);
		rpal_put_service(rs);
	}
}
