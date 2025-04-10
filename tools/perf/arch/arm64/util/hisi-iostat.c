// SPDX-License-Identifier: GPL-2.0
/*
 * perf iostat support for HiSilicon PCIe PMU.
 * Partly derived from tools/perf/arch/x86/util/iostat.c.
 *
 * Copyright (c) 2024 HiSilicon Technologies Co., Ltd.
 * Author: Yicong Yang <yangyicong@hisilicon.com>
 */

#include <api/fs/fs.h>
#include <linux/err.h>
#include <linux/zalloc.h>
#include <linux/limits.h>
#include <dirent.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>

#include "util/counts.h"
#include "util/cpumap.h"
#include "util/debug.h"
#include "util/iostat.h"
#include "util/pmu.h"

#define PCI_DEFAULT_DOMAIN		0
#define PCI_DEVICE_NAME_PATTERN		"%04x:%02hhx:%02hhx.%hhu"
#define PCI_ROOT_BUS_DEVICES_PATH	"bus/pci/devices"

static const char *hisi_iostat_metrics[] = {
	"Inbound MWR(MB)",
	"Inbound MRD(MB)",
	"Inbound CPL(MB)",
	"Outbound MWR(MB)",
	"Outbound MRD(MB)",
	"Outbound CPL(MB)",
};

static const char *hisi_iostat_cmd_template[] = {
	/* Inbound Memory Write */
	"hisi_pcie%hu_core%hu/event=0x0104,port=0x%hx/",
	/* Inbound Memory Read */
	"hisi_pcie%hu_core%hu/event=0x0804,port=0x%hx/",
	/* Inbound Memory Completion */
	"hisi_pcie%hu_core%hu/event=0x2004,port=0x%hx/",
	/* Outbound Memory Write */
	"hisi_pcie%hu_core%hu/event=0x0105,port=0x%hx/",
	/* Outbound Memory Read */
	"hisi_pcie%hu_core%hu/event=0x0405,port=0x%hx/",
	/* Outbound Memory Completion */
	"hisi_pcie%hu_core%hu/event=0x1005,port=0x%hx/",
};

struct hisi_pcie_root_port {
	struct list_head list;
	/* Is this Root Port selected for monitoring */
	bool selected;
	/* IDs to locate the PMU */
	u16 sicl_id;
	u16 core_id;
	/* Filter mask for this Root Port */
	u16 mask;
	/* PCIe Root Port's <domain>:<bus>:<device>.<function> */
	u32 domain;
	u8 bus;
	u8 dev;
	u8 fn;
};

LIST_HEAD(hisi_pcie_root_ports_list);
static int hisi_pcie_root_ports_num;

static void hisi_pcie_init_root_port_mask(struct hisi_pcie_root_port *rp)
{
	rp->mask = BIT(rp->dev << 1);
}

/*
 * Select specific Root Port to monitor. Return 0 if successfully find the
 * Root Port, Otherwise -EINVAL.
 */
static int hisi_pcie_root_ports_select_one(u32 domain, u8 bus, u8 dev, u8 fn)
{
	struct hisi_pcie_root_port *rp;

	list_for_each_entry(rp, &hisi_pcie_root_ports_list, list)
		if (domain == rp->domain && bus == rp->bus &&
		    dev == rp->dev && fn == rp->fn) {
			rp->selected = true;
			return 0;
		}

	return -EINVAL;
}

static void hisi_pcie_root_ports_select_all(void)
{
	struct hisi_pcie_root_port *rp;

	list_for_each_entry(rp, &hisi_pcie_root_ports_list, list)
		rp->selected = true;
}

static void hisi_pcie_root_ports_add(u16 sicl_id, u16 core_id, u8 target_bus)
{
	const char *sysfs = sysfs__mountpoint();
	struct hisi_pcie_root_port *rp;
	struct dirent *dent;
	char path[PATH_MAX];
	u8 bus, dev, fn;
	u32 domain;
	DIR *dir;
	int ret;

	snprintf(path, PATH_MAX, "%s/%s", sysfs, PCI_ROOT_BUS_DEVICES_PATH);
	dir = opendir(path);
	if (!dir)
		return;

	/* Scan the PCI root bus to find the match root port on @target_bus */
	while ((dent = readdir(dir))) {
		ret = sscanf(dent->d_name, PCI_DEVICE_NAME_PATTERN,
			     &domain, &bus, &dev, &fn);
		if (ret != 4 || bus != target_bus)
			continue;

		rp = zalloc(sizeof(*rp));
		if (!rp)
			continue;

		rp->selected = false;
		rp->sicl_id = sicl_id;
		rp->core_id = core_id;
		rp->domain = domain;
		rp->bus = bus;
		rp->dev = dev;
		rp->fn = fn;

		hisi_pcie_init_root_port_mask(rp);

		list_add(&rp->list, &hisi_pcie_root_ports_list);
		hisi_pcie_root_ports_num++;

		pr_debug3("Found root port %s\n", dent->d_name);
	}

	closedir(dir);
}

/* Scan the PMUs and build the mapping of the Root Ports to the PMU */
static int hisi_pcie_root_ports_init(void)
{
	char event_source[PATH_MAX], bus_path[PATH_MAX];
	unsigned long long bus;
	u16 sicl_id, core_id;
	struct dirent *dent;
	DIR *dir;

	perf_pmu__event_source_devices_scnprintf(event_source, sizeof(event_source));
	dir = opendir(event_source);
	if (!dir)
		return -ENOENT;

	while ((dent = readdir(dir))) {
		/*
		 * This HiSilicon PCIe PMU will be named as:
		 *   hisi_pcie<sicl_id>_core<core_id>
		 */
		if ((sscanf(dent->d_name, "hisi_pcie%hu_core%hu", &sicl_id, &core_id)) != 2)
			continue;

		/*
		 * Driver will export the root port it can monitor through
		 * the "bus" sysfs attribute.
		 */
		scnprintf(bus_path, sizeof(bus_path), "%s/hisi_pcie%hu_core%hu/bus",
			  event_source, sicl_id, core_id);

		/*
		 * Per PCIe spec the bus should be 8bit, use unsigned long long
		 * for the convience of the library function.
		 */
		if (filename__read_ull(bus_path, &bus))
			continue;

		pr_debug3("Found pmu %s bus 0x%llx\n", dent->d_name, bus);

		hisi_pcie_root_ports_add(sicl_id, core_id, (u8)bus);
	}

	closedir(dir);
	return hisi_pcie_root_ports_num > 0 ? 0 : -ENOENT;
}

static void hisi_pcie_root_ports_free(void)
{
	struct hisi_pcie_root_port *rp, *tmp;

	if (hisi_pcie_root_ports_num == 0)
		return;

	list_for_each_entry_safe(rp, tmp, &hisi_pcie_root_ports_list, list) {
		list_del(&rp->list);
		zfree(&rp);
		hisi_pcie_root_ports_num--;
	}
}

static int hisi_iostat_add_events(struct evlist *evl)
{
	struct hisi_pcie_root_port *rp;
	struct evsel *evsel;
	unsigned int i, j;
	char *iostat_cmd;
	int pos = 0;
	int ret;

	if (!hisi_pcie_root_ports_num)
		return -ENOENT;

	iostat_cmd = zalloc(PATH_MAX);
	if (!iostat_cmd)
		return -ENOMEM;

	list_for_each_entry(rp, &hisi_pcie_root_ports_list, list) {
		if (!rp->selected)
			continue;

		iostat_cmd[pos++] = '{';
		for (j = 0; j < ARRAY_SIZE(hisi_iostat_cmd_template); j++) {
			pos += snprintf(iostat_cmd + pos, ARG_MAX - pos - 1,
					hisi_iostat_cmd_template[j],
					rp->sicl_id, rp->core_id, rp->mask);

			if (j == ARRAY_SIZE(hisi_iostat_cmd_template) - 1)
				iostat_cmd[pos++] = '}';
			else
				iostat_cmd[pos++] = ',';
		}

		ret = parse_event(evl, iostat_cmd);
		if (ret)
			break;

		i = 0;
		evlist__for_each_entry_reverse(evl, evsel) {
			if (i == ARRAY_SIZE(hisi_iostat_cmd_template))
				break;

			evsel->priv = rp;
			i++;
		}

		memset(iostat_cmd, 0, PATH_MAX);
		pos = 0;
	}

	zfree(&iostat_cmd);
	return ret;
}

int iostat_prepare(struct evlist *evlist,
		   struct perf_stat_config *config)
{
	if (evlist->core.nr_entries > 0) {
		pr_warning("The -e and -M options are not supported."
			   "All chosen events/metrics will be dropped\n");
		evlist__delete(evlist);
		evlist = evlist__new();
		if (!evlist)
			return -ENOMEM;
	}

	config->metric_only = true;
	config->aggr_mode = AGGR_GLOBAL;

	return hisi_iostat_add_events(evlist);
}

static int hisi_pcie_root_ports_list_filter(const char *str)
{
	char *tok, *tmp, *copy = NULL;
	u8 bus, dev, fn;
	u32 domain;
	int ret;

	copy = strdup(str);
	if (!copy)
		return -ENOMEM;

	for (tok = strtok_r(copy, ",", &tmp); tok; tok = strtok_r(NULL, ",", &tmp)) {
		ret = sscanf(tok, PCI_DEVICE_NAME_PATTERN, &domain, &bus, &dev, &fn);
		if (ret != 4) {
			ret = -EINVAL;
			break;
		}

		ret = hisi_pcie_root_ports_select_one(domain, bus, dev, fn);
		if (ret)
			break;
	}

	zfree(&copy);
	return ret;
}

int iostat_parse(const struct option *opt, const char *str, int unset __maybe_unused)
{
	struct perf_stat_config *config = (struct perf_stat_config *)opt->data;
	int ret;

	ret = hisi_pcie_root_ports_init();
	if (!ret) {
		config->iostat_run = true;

		if (!str) {
			iostat_mode = IOSTAT_RUN;
			hisi_pcie_root_ports_select_all();
		} else if (!strcmp(str, "list")) {
			iostat_mode = IOSTAT_LIST;
			hisi_pcie_root_ports_select_all();
		} else {
			iostat_mode = IOSTAT_RUN;
			ret = hisi_pcie_root_ports_list_filter(str);
		}
	}

	return ret;
}

static void hisi_pcie_root_port_show(FILE *output,
				     const struct hisi_pcie_root_port * const rp)
{
	if (output && rp)
		fprintf(output, "hisi_pcie%hu_core%hu<" PCI_DEVICE_NAME_PATTERN ">\n",
			rp->sicl_id, rp->core_id, rp->domain, rp->bus, rp->dev, rp->fn);
}

void iostat_list(struct evlist *evlist __maybe_unused, struct perf_stat_config *config)
{
	struct hisi_pcie_root_port *rp = NULL;
	struct evsel *evsel;

	evlist__for_each_entry(evlist, evsel) {
		if (rp != evsel->priv) {
			hisi_pcie_root_port_show(config->output, evsel->priv);
			rp = evsel->priv;
		}
	}
}

void iostat_release(struct evlist *evlist)
{
	struct evsel *evsel;

	evlist__for_each_entry(evlist, evsel)
		evsel->priv = NULL;

	hisi_pcie_root_ports_free();
}

void iostat_print_header_prefix(struct perf_stat_config *config)
{
	if (config->csv_output)
		fputs("port,", config->output);
	else if (config->interval)
		fprintf(config->output, "#          time    port         ");
	else
		fprintf(config->output, "   port         ");
}

void iostat_print_metric(struct perf_stat_config *config, struct evsel *evsel,
			 struct perf_stat_output_ctx *out)
{
	const char *iostat_metric = hisi_iostat_metrics[evsel->core.idx % ARRAY_SIZE(hisi_iostat_metrics)];
	struct perf_counts_values *count;
	double iostat_value;

	/* We're using AGGR_GLOBAL so there's only one aggr counts aggr[0]. */
	count = &evsel->stats->aggr[0].counts;

	/* The counts has been scaled, we can use it directly. */
	iostat_value = (double)count->val;

	/*
	 * Display two digits after decimal point for better accuracy if the
	 * value is non-zero.
	 */
	out->print_metric(config, out->ctx, NULL,
			  iostat_value > 0 ? "%8.2f" : "%8.0f",
			  iostat_metric, iostat_value / (256 * 1024));
}

void iostat_prefix(struct evlist *evlist, struct perf_stat_config *config,
		   char *prefix, struct timespec *ts)
{
	struct hisi_pcie_root_port *rp = evlist->selected->priv;

	if (rp) {
		if (ts)
			sprintf(prefix, "%6lu.%09lu%s" PCI_DEVICE_NAME_PATTERN "%s",
				ts->tv_sec, ts->tv_nsec, config->csv_sep,
				rp->domain, rp->bus, rp->dev, rp->fn,
				config->csv_sep);
		else
			sprintf(prefix, PCI_DEVICE_NAME_PATTERN "%s",
				rp->domain, rp->bus, rp->dev, rp->fn,
				config->csv_sep);
	}
}

void iostat_print_counters(struct evlist *evlist, struct perf_stat_config *config,
			   struct timespec *ts, char *prefix,
			   iostat_print_counter_t print_cnt_cb, void *arg)
{
	struct evsel *counter = evlist__first(evlist);
	void *perf_device;

	evlist__set_selected(evlist, counter);
	iostat_prefix(evlist, config, prefix, ts);
	fprintf(config->output, "%s", prefix);
	evlist__for_each_entry(evlist, counter) {
		perf_device = evlist->selected->priv;
		if (perf_device && perf_device != counter->priv) {
			evlist__set_selected(evlist, counter);
			iostat_prefix(evlist, config, prefix, ts);
			fprintf(config->output, "\n%s", prefix);
		}
		print_cnt_cb(config, counter, arg);
	}
	fputc('\n', config->output);
}
