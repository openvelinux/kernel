/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright(c) 2022 Huawei Technologies Co., Ltd
 */

#ifndef __HISI_VIRT_H__
#define __HISI_VIRT_H__

#ifdef CONFIG_KVM_HISI_VIRT
enum hisi_cpu_type {
	HI_1612,
	HI_1616,
	HI_1620,
	HI_IP09,
	HI_IP10,
	HI_IP10C,
	UNKNOWN_HI_TYPE
};

/* HIP10 */
#define AIDR_EL1_DVMBM_MASK	GENMASK_ULL(13, 12)
#define SYS_LSUDVM_CTRL_EL2	sys_reg(3, 4, 15, 7, 4)
#define LSUDVM_CTLR_EL2_MASK	BIT_ULL(0)

#define MAX_CLUSTERS_PER_DIE 8
#define TB_MN_BASE 0x00C6067f0000
#define TA_MN_BASE 0x0046067F0000
#define CHIP_ADDR_OFFSET(_chip)        (((((_chip) >> 3) & 0x1) * 0x80000000000) + \
	((((_chip) >> 2) & 0x1) * (0x100000000000)) + \
	(((_chip) & 0x3) * 0x200000000000))
#define MAX_PG_CFG_SOCKETS 4
#define MAX_DIES_PER_SOCKET 2
#define MN_ECO0_OFFSET 0xc00
#define SOCKET_ID_MASK 0xf8
#define SOCKET_ID_SHIFT 3
#define DIE_ID_MASK 0x7
#define DIE_ID_SHIFT 0
#define TOTEM_B_ID 3

/*
 * MPIDR_EL1 layout on HIP10
 *
 * Aff3[7:3]	- socket ID	[0-15]
 * Aff3[2:0]	- die ID	[1,3]
 * Aff2		- cluster ID	[0-9]
 * Aff1		- core ID	[0-3]
 * Aff0		- thread ID	[0,1]
 */

#define SYS_LSUDVMBM_EL2		sys_reg(3, 4, 15, 7, 5)
#define DVMBM_RANGE_SHIFT		62
#define DVMBM_RANGE_ONE_DIE		0ULL
#define DVMBM_RANGE_TWO_DIES		1ULL
#define DVMBM_RANGE_ALL_DIES		3ULL

#define DVMBM_GRAN_SHIFT		61
#define DVMBM_GRAN_CLUSTER		0ULL
#define DVMBM_GRAN_DIE			1ULL

#define DVMBM_DIE1_SHIFT		53
#define DVMBM_DIE2_SHIFT		45
#define DVMBM_DIE1_CLUSTER_SHIFT	22
#define DVMBM_DIE2_CLUSTER_SHIFT	0

#define DVMBM_MAX_DIES			32

void probe_hisi_cpu_type(void);
bool hisi_ncsnp_supported(void);
bool hisi_dvmbm_supported(void);
void kvm_get_pg_cfg(void);

int kvm_sched_affinity_vcpu_init(struct kvm_vcpu *vcpu);
void kvm_sched_affinity_vcpu_destroy(struct kvm_vcpu *vcpu);
int kvm_sched_affinity_vm_init(struct kvm *kvm);
void kvm_sched_affinity_vm_destroy(struct kvm *kvm);
void kvm_tlbi_dvmbm_vcpu_load(struct kvm_vcpu *vcpu);
void kvm_tlbi_dvmbm_vcpu_put(struct kvm_vcpu *vcpu);
void kvm_hisi_reload_lsudvmbm(struct kvm *kvm);
#else
static inline void probe_hisi_cpu_type(void) {}
static inline bool hisi_ncsnp_supported(void)
{
	return false;
}
static inline bool hisi_dvmbm_supported(void)
{
	return false;
}
static inline void kvm_get_pg_cfg(void) {}

static inline int kvm_sched_affinity_vcpu_init(struct kvm_vcpu *vcpu)
{
	return 0;
}
static inline void kvm_sched_affinity_vcpu_destroy(struct kvm_vcpu *vcpu) {}
static inline int kvm_sched_affinity_vm_init(struct kvm *kvm)
{
	return 0;
}
static inline void kvm_sched_affinity_vm_destroy(struct kvm *kvm) {}
static inline void kvm_tlbi_dvmbm_vcpu_load(struct kvm_vcpu *vcpu) {}
static inline void kvm_tlbi_dvmbm_vcpu_put(struct kvm_vcpu *vcpu) {}
static inline void kvm_hisi_reload_lsudvmbm(struct kvm *kvm) {}
#endif /* CONFIG_KVM_HISI_VIRT */

#endif /* __HISI_VIRT_H__ */
