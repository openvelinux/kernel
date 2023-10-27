// SPDX-License-Identifier: GPL-2.0
#include <linux/pid_namespace.h>
#include <linux/sched.h>
#include <linux/proc_fs.h>
#include <linux/cgroup.h>
#include <linux/cpuset.h>
#include <linux/sched/task.h>
#include <linux/memcontrol.h>

int sysctl_cgroup_override_proc;
int sysctl_cgroup_override_source;

bool cgroup_override_proc(void)
{
	struct task_struct *init_tsk;
	if (sysctl_cgroup_override_proc == 0)
		return false;

	init_tsk = cgroup_override_get_source_tsk();

	rcu_read_lock();
	if (task_active_pid_ns(current) == &init_pid_ns ||
	    !cpuacct_not_root(current) ||
	    !tg_get_override_proc(init_tsk->sched_task_group)) {
		put_task_struct(init_tsk);
		rcu_read_unlock();
		return false;
	}
	put_task_struct(init_tsk);

	rcu_read_unlock();
	return true;
}

struct task_struct *cgroup_override_get_source_tsk(void)
{
	struct task_struct *cg_init_tsk;

	rcu_read_lock();
	if (sysctl_cgroup_override_source)
		cg_init_tsk = task_active_pid_ns(current)->child_reaper;
	else
		cg_init_tsk = current;

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
	struct task_struct *cg_init_tsk = cgroup_override_get_source_tsk();
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

void cgroup_override_kcpustat(struct kernel_cpustat *kcs,
			      struct kernel_cpustat *raw_kcs)
{
	u64 ca_usage, raw_usage;
	int i;

	ca_usage = kcs->cpustat[CPUTIME_NICE] +
		   kcs->cpustat[CPUTIME_USER] +
		   kcs->cpustat[CPUTIME_SYSTEM];
	raw_usage = raw_kcs->cpustat[CPUTIME_NICE] +
		    raw_kcs->cpustat[CPUTIME_USER] +
		    raw_kcs->cpustat[CPUTIME_SYSTEM];

	for (i = CPUTIME_SOFTIRQ; i < NR_STATS; i++)
		kcs->cpustat[i] = raw_kcs->cpustat[i];

	/* treat cpu used by a process on host as steal, like VM. */
	kcs->cpustat[CPUTIME_STEAL] += raw_usage - ca_usage;
}

struct mem_cgroup *cgroup_override_get_memcg(void)
{
	struct mem_cgroup *memcg;
	struct task_struct *tsk = cgroup_override_get_source_tsk();

	memcg = mem_cgroup_from_task(tsk);
	if (mem_cgroup_is_root(memcg))
		memcg = NULL;
	else
		css_get(&memcg->css);
	put_task_struct(tsk);

	return memcg;
}

void cgroup_override_meminfo(struct sysinfo *info, struct mem_cgroup *memcg)
{
	unsigned long mem_limit, memsw_limit, mem_usage;
	struct mem_cgroup *tmp;

	mem_limit = memsw_limit = PAGE_COUNTER_MAX;

	for (tmp = memcg; tmp; tmp = parent_mem_cgroup(tmp)) {
		mem_limit = min(mem_limit, tmp->memory.max);
		memsw_limit = min(memsw_limit, tmp->memsw.max);
	}

	if (mem_cgroup_is_root(memcg))
		mem_usage = memcg_page_state(memcg, NR_FILE_PAGES) +
			    memcg_page_state(memcg, NR_ANON_MAPPED);
	else
		mem_usage = page_counter_read(&memcg->memory);

	info->totalram =
		mem_limit > info->totalram ? info->totalram : mem_limit;
	info->freeram = info->totalram - mem_usage;
	info->sharedram = memcg_page_state(memcg, NR_SHMEM);
	info->bufferram = 0;
	info->totalhigh = totalhigh_pages();
	info->freehigh = nr_free_highpages();
	if (cgroup_memory_noswap) {
		info->totalswap = 0;
		info->freeswap = 0;
	} else {
		info->totalswap = info->totalswap > memsw_limit ?
					  memsw_limit :
					  info->totalswap;
		info->freeswap =
			info->totalswap - memcg_page_state(memcg, MEMCG_SWAP);
	}

}
