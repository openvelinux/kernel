/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MM_GUESTMEM_HUGETLB_H
#define _LINUX_MM_GUESTMEM_HUGETLB_H

#include <linux/mm_types.h>

void guestmem_hugetlb_handle_folio_put(struct folio *folio);

#endif
