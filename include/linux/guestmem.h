/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_GUESTMEM_H
#define _LINUX_GUESTMEM_H

#include <linux/fs.h>

struct guestmem_allocator_operations {
	void *(*inode_setup)(size_t size, u64 flags);
	void (*inode_teardown)(void *private, size_t inode_size);
	struct folio *(*alloc_folio)(void *private, struct mempolicy *mpol);
	int (*split_folio)(struct folio *folio);
	void (*merge_folio)(struct folio *folio);
	void (*free_folio)(struct folio *folio);
	/*
	 * Returns the number of PAGE_SIZE pages in a page that this guestmem
	 * allocator provides.
	 */
	size_t (*nr_pages_in_folio)(void *priv);
	bool (*skip_merge_folio)(struct folio *folio);
};

extern const struct guestmem_allocator_operations guestmem_hugetlb_ops;

#endif
