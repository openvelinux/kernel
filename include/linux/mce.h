/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MCE_H
#define _LINUX_MCE_H

#include <linux/sched.h>

#ifdef CONFIG_X86_MCE
extern void mcestat_record(struct task_struct *task,
			   unsigned long addr, int signal, bool cmci);
#else
static void mcestat_record(struct task_struct *task,
			   unsigned long addr, int signal, bool cmci)
{

}
#endif

#endif
