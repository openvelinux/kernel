/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cpuinspect.h - The internal header file
 */

#ifndef __DRIVER_CPUINSPECT_H
#define __DRIVER_CPUINSPECT_H

#define CPUINSPECT_NAME_LEN 16

/* sysfs */
int cpuinspect_add_interface(void);
void cpuinspect_remove_interface(void);
void cpuinspect_result_notify(void);

/* inspect control */
int start_inspect_threads(void);
int stop_inspect_threads(void);
int cpuinspect_is_running(void);

/* switch inspector */
int cpuinspect_switch_inspector(struct cpu_inspector *insp);

/* for internal use only */
extern DECLARE_BITMAP(result, NR_CPUS);
extern struct cpu_inspector *curr_cpu_inspector;
extern struct mutex cpuinspect_lock;
extern struct cpuinspect ci_core;
extern struct list_head cpu_inspectors;
extern char param_inspector[];

/**
 * struct cpuinspect - the basic cpuinspect structure
 * @cpu_utility:	Maximum CPU utilization occupied by the inspection thread.
 * @inspect_times:	The number of times the inspection code will be executed.
 * @inspect_cpumask:	cpumask to indicate for which CPUs are involved in inspection.
 * @inspect_on:		Set if the inspection thread is running.
 */
struct cpuinspect {
	unsigned int	cpu_utility;
	unsigned long	inspect_times;
	int		inspect_on;
	cpumask_t	inspect_cpumask;
};

#endif /* __DRIVER_CPUINSPECT_H */
