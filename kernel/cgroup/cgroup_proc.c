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
