/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2022 ARM Ltd.
 */
#ifndef __ASM_NMI_H
#define __ASM_NMI_H


static __always_inline void _allint_clear(void)
{
	asm volatile(__msr_s(SYS_ALLINT_CLR, "xzr"));
}

static __always_inline void _allint_set(void)
{
	asm volatile(__msr_s(SYS_ALLINT_SET, "xzr"));
}

#endif
