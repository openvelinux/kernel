// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/time.h>
#include <linux/time_namespace.h>
#include <linux/kernel_stat.h>
#include <linux/cgroup.h>
#include <linux/sched/task.h>

static int uptime_proc_show(struct seq_file *m, void *v)
{
	struct cpumask msk;
	struct kernel_cpustat kcpustat;
	struct task_struct *tsk;
	struct timespec64 uptime;
	struct timespec64 idle;
	u64 nsec = 0;
	u32 rem;
	int i;

	ktime_get_boottime_ts64(&uptime);
	timens_add_boottime(&uptime);

	if (cgroup_override_proc()) {
		tsk = cgroup_override_get_init_tsk();
		uptime = timespec64_sub(uptime,
					ns_to_timespec64(tsk->start_time));

		cgroup_override_get_raw_cpuset(&msk);
		for_each_cpu(i, &msk) {
			if (cpuacct_get_kcpustat(tsk, i, &kcpustat))
				nsec += (__force u64)kcpustat.cpustat[CPUTIME_IDLE];
		}
		put_task_struct(tsk);
	} else {
		for_each_possible_cpu(i) {
			struct kernel_cpustat kcs;

			kcpustat_cpu_fetch(&kcs, i);
			nsec += get_idle_time(&kcs, i);
		}
	}

	idle.tv_sec = div_u64_rem(nsec, NSEC_PER_SEC, &rem);
	idle.tv_nsec = rem;
	seq_printf(m, "%lu.%02lu %lu.%02lu\n",
			(unsigned long) uptime.tv_sec,
			(uptime.tv_nsec / (NSEC_PER_SEC / 100)),
			(unsigned long) idle.tv_sec,
			(idle.tv_nsec / (NSEC_PER_SEC / 100)));
	return 0;
}

static int __init proc_uptime_init(void)
{
	proc_create_single("uptime", 0, NULL, uptime_proc_show);
	return 0;
}
fs_initcall(proc_uptime_init);
