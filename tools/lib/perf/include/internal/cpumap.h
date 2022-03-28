/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LIBPERF_INTERNAL_CPUMAP_H
#define __LIBPERF_INTERNAL_CPUMAP_H

#include <linux/refcount.h>
#include <perf/cpumap.h>

struct perf_cpu_map {
	refcount_t	refcnt;
	int		nr;
	struct perf_cpu	map[];
};

#ifndef MAX_NR_CPUS
#define MAX_NR_CPUS	2048
#endif

int perf_cpu_map__idx(struct perf_cpu_map *cpus, int cpu);
bool perf_cpu_map__is_subset(const struct perf_cpu_map *a, const struct perf_cpu_map *b);

#endif /* __LIBPERF_INTERNAL_CPUMAP_H */
