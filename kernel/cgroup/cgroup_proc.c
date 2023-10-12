// SPDX-License-Identifier: GPL-2.0
#include <linux/pid_namespace.h>
#include <linux/sched.h>
#include <linux/proc_fs.h>
#include <linux/cgroup.h>
#include <linux/cpuset.h>
#include <linux/sched/task.h>

int sysctl_cgroup_override_proc;

bool cgroup_override_proc(void)
{
	if (sysctl_cgroup_override_proc == 0)
		return false;

	rcu_read_lock();
	if (task_active_pid_ns(current) == &init_pid_ns || !cpuacct_not_root(current)) {
		rcu_read_unlock();
		return false;
	}

	rcu_read_unlock();
	return true;
}

struct task_struct *cgroup_override_get_init_tsk(void)
{
	struct task_struct *cg_init_tsk;

	rcu_read_lock();
	cg_init_tsk = task_active_pid_ns(current)->child_reaper;
	get_task_struct(cg_init_tsk);
	rcu_read_unlock();

	return cg_init_tsk;
}

/**
 * The value of cpuset is preferred, and if not configured, the ratio of cpu_quota
 * to cpu_period is used.
 */
void cgroup_override_get_raw_cpuset(struct cpumask *cpuset)
{
	long quota, period;
	int cpus, cpu_index, count = 0;
	struct task_struct *cg_init_tsk = cgroup_override_get_init_tsk();
	struct task_group *sched_task_group;

	cpuset_cpus_allowed(cg_init_tsk, cpuset);

	rcu_read_lock();

	sched_task_group = cg_init_tsk->sched_task_group;

	quota = tg_get_cfs_quota(sched_task_group);
	period = tg_get_cfs_period(sched_task_group);

	rcu_read_unlock();

	if (quota != -1 && period != -1) {
		/* period can't be 0 */
		cpus = (quota + period - 1) / period;
		cpus = clamp(cpus, 1, (int)num_online_cpus());

		/* otherwise direct use cpuset. */
		if (cpumask_weight(cpuset) > cpus) {
			for_each_cpu(cpu_index, cpuset) {
				if (count++ < cpus)
					continue;
				cpumask_clear_cpu(cpu_index, cpuset);
			}
		}
	}

	put_task_struct(cg_init_tsk);
}

/**
 * Get the mapped value. Such as 2-5 -> 0-3
 */
void cgroup_override_get_cpuset(struct cpumask *cpuset)
{
	struct cpumask tmp;

	cgroup_override_get_raw_cpuset(&tmp);

	cpumask_clear(cpuset);
	bitmap_set(cpumask_bits(cpuset), 0, cpumask_weight(&tmp));
}
