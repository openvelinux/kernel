/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MCE_H
#define _LINUX_MCE_H

#include <linux/sched.h>

#ifdef CONFIG_BYTEDANCE_X86_MCE_STAT
extern void mcestat_record(struct task_struct *task,
			   unsigned long addr, int signal, bool cmci);
#else
static void mcestat_record(struct task_struct *task,
			   unsigned long addr, int signal, bool cmci)
{

}
#endif

#if IS_ENABLED(CONFIG_KVM)
extern bool mce_kvm __read_mostly;
extern bool mce_kill_kvm __read_mostly;
#endif

#endif
