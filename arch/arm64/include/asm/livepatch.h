/* SPDX-License-Identifier: GPL-2.0
 *
 * livepatch.h - arm64-specific Kernel Live Patching Core
 *
 * Copyright (C) 2016,2018 SUSE
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */
#ifndef _ASM_ARM64_LIVEPATCH_H
#define _ASM_ARM64_LIVEPATCH_H

#include <linux/module.h>
#include <linux/ftrace.h>

#ifdef CONFIG_LIVEPATCH

#ifdef CONFIG_DYNAMIC_FTRACE_WITH_REGS
static inline void klp_arch_set_pc(struct ftrace_regs *fregs, unsigned long pc)
{
	fregs->regs.pc = pc;

}

#define klp_get_ftrace_location klp_get_ftrace_location
static inline unsigned long klp_get_ftrace_location(unsigned long faddr)
{
	return faddr + AARCH64_INSN_SIZE;
}

#else
static inline void klp_arch_set_pc(struct ftrace_regs *fregs, unsigned long pc)
{
}

#define klp_get_ftrace_location klp_get_ftrace_location
static inline unsigned long klp_get_ftrace_location(unsigned long faddr)
{
	return faddr + AARCH64_INSN_SIZE;
}
#endif

#else
#error Live patching support is disabled; check CONFIG_LIVEPATCH
#endif

#endif /* _ASM_ARM64_LIVEPATCH_H */
