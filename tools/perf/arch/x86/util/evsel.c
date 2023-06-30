// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>
#include "util/evsel.h"
#include "util/debug.h"
#include "linux/string.h"
#include "util/pmu.h"
#include "util/env.h"
#include "env.h"

#define IBS_FETCH_L3MISSONLY   (1ULL << 59)
#define IBS_OP_L3MISSONLY      (1ULL << 16)

void arch_evsel__set_sample_weight(struct evsel *evsel)
{
	evsel__set_sample_bit(evsel, WEIGHT_STRUCT);
}

static void ibs_l3miss_warn(void)
{
	pr_warning(
"WARNING: Hw internally resets sampling period when L3 Miss Filtering is enabled\n"
"and tagged operation does not cause L3 Miss. This causes sampling period skew.\n");
}

void arch__post_evsel_config(struct evsel *evsel, struct perf_event_attr *attr)
{
	struct perf_pmu *evsel_pmu, *ibs_fetch_pmu, *ibs_op_pmu;
	static int warned_once;

	if (warned_once || !x86__is_amd_cpu())
		return;

	evsel_pmu = evsel__find_pmu(evsel);
	if (!evsel_pmu)
		return;

	ibs_fetch_pmu = perf_pmu__find("ibs_fetch");
	ibs_op_pmu = perf_pmu__find("ibs_op");

	if (ibs_fetch_pmu && ibs_fetch_pmu->type == evsel_pmu->type) {
		if (attr->config & IBS_FETCH_L3MISSONLY) {
			ibs_l3miss_warn();
			warned_once = 1;
		}
	} else if (ibs_op_pmu && ibs_op_pmu->type == evsel_pmu->type) {
		if (attr->config & IBS_OP_L3MISSONLY) {
			ibs_l3miss_warn();
			warned_once = 1;
		}
	}
}

int arch_evsel__open_strerror(struct evsel *evsel, char *msg, size_t size)
{
	if (!x86__is_amd_cpu())
		return 0;

	if (!evsel->core.attr.precise_ip &&
	    !(evsel->pmu_name && !strncmp(evsel->pmu_name, "ibs", 3)))
		return 0;

	/* More verbose IBS errors. */
	if (evsel->core.attr.exclude_kernel || evsel->core.attr.exclude_user ||
	    evsel->core.attr.exclude_hv || evsel->core.attr.exclude_idle ||
	    evsel->core.attr.exclude_host || evsel->core.attr.exclude_guest) {
		return scnprintf(msg, size, "AMD IBS doesn't support privilege filtering. Try "
				 "again without the privilege modifiers (like 'k') at the end.");
	}

	return 0;
}
