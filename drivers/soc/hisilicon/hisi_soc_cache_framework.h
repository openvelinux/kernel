/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Header file of framework for HiSilicon SoC cache.
 *
 * Copyright (c) 2024 HiSilicon Technologies Co., Ltd.
 * Author: Jie Wang <wangjie125@huawei.com>
 * Author: Yicong Yang <yangyicong@hisilicon.com>
 * Author: Yushan Wang <wangyushan12@huawei.com>
 */

#ifndef HISI_CACHE_FRAMEWORK_H
#define HISI_CACHE_FRAMEWORK_H

#include <linux/bits.h>
#include <linux/types.h>

#include <uapi/misc/hisi_soc_cache/hisi_soc_cache.h>

enum hisi_soc_comp_type {
	HISI_SOC_HHA,
	SOC_COMP_TYPE_MAX
};

struct hisi_soc_comp;

/**
 * struct hisi_soc_comp_ops - Callbacks for SoC cache drivers to handle
 *			      operation requests.
 * @maintain_enable: perform certain cache maintain operation on HHA.
 * @poll_maintain_done: check if the HHA maintain operation has succeeded.
 *
 * Operations are decoupled into two phases so that framework does not have
 * to wait for one operation to finish before calling the next when multiple
 * hardwares onboard.
 *
 * Implementers must implement the functions in pairs.  Implementation should
 * return -EBUSY when:
 * - insufficient resources are available to perform the operation.
 * - previously raised operation is not finished.
 * - new operations (do_lock(), do_unlock() etc.) to the same address
 *   before corresponding done functions being called.
 */
struct hisi_soc_comp_ops {
	int (*do_maintain)(struct hisi_soc_comp *comp,
			      phys_addr_t addr, size_t size,
			      enum hisi_soc_cache_maint_type mnt_type);
	int (*poll_maintain_done)(struct hisi_soc_comp *comp,
			      phys_addr_t addr, size_t size,
			      enum hisi_soc_cache_maint_type mnt_type);
};

/**
 * struct hisi_soc_comp - Struct of HiSilicon SoC cache components.
 * @ops: possible operations a component may perform.
 * @affinity_mask: cpus that associate with this component.
 * @comp_type: bitmap declaring the type of the component.
 *
 * A component may have multiple types (e.g. a piece of multi-function device).
 * If so, set the bit of @comp_type according to its supporting type in struct
 * hisi_soc_comp_type.
 */
struct hisi_soc_comp {
	struct hisi_soc_comp_ops *ops;
	cpumask_t affinity_mask;
	/*
	 * Setting bit x to 1 means this instance supports feature of x-th
	 * entry in enum hisi_soc_comp_type.
	 */
	unsigned long comp_type;
};

int hisi_soc_comp_inst_add(struct hisi_soc_comp *comp);
int hisi_soc_comp_inst_del(struct hisi_soc_comp *comp);
int hisi_soc_cache_maintain(phys_addr_t addr, size_t size,
			    enum hisi_soc_cache_maint_type mnt_type);

#endif
