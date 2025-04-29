// SPDX-License-Identifier: GPL-2.0-only
/*
 * guestmem_hugetlb is an allocator for guest_memfd. guest_memfd wraps HugeTLB
 * as an allocator for guest_memfd.
 */

#include <linux/mm_types.h>

#include "guestmem_hugetlb.h"

void guestmem_hugetlb_handle_folio_put(struct folio *folio)
{
	WARN_ONCE(1, "A placeholder that shouldn't trigger. Work in progress.");
}
