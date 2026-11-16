// SPDX-License-Identifier: GPL-2.0
#include <linux/hugetlb.h>
#include <linux/export.h>
#include <linux/acpi.h>
#include <linux/irqchip/arm-gic-common.h>
#include <linux/irqchip/arm-gic-v4.h>
#include <asm/mmu_context.h>
#include <asm/traps.h>
#include <asm/vectors.h>
#include <asm/sections.h>
#include <linux/perf/arm_pmu.h>
#include <asm/kvm_pgtable.h>
#include <asm/arm_pmuv3.h>

#include <clocksource/arm_arch_timer.h>
#include <asm/smp.h>
#include <asm/spectre.h>
#include <linux/irqdomain.h>
#include <linux/smp.h>
#include <asm/fpsimd.h>
#include <asm/cpufeature.h>
#include <clocksource/arm_arch_timer.h>

DEFINE_STATIC_KEY_FALSE(kvm_protected_mode_initialized);
EXPORT_SYMBOL(kvm_protected_mode_initialized);

#if IS_MODULE(CONFIG_KVM)

/*
 * Export required symbols for modularized kvm
 */
EXPORT_SYMBOL(aarch64_insn_gen_movewide);
EXPORT_SYMBOL(fpsimd_save_and_flush_cpu_state);
EXPORT_SYMBOL(vectors);
EXPORT_SYMBOL(this_cpu_vector);
EXPORT_SYMBOL(arm64_get_spectre_bhb_state);
EXPORT_SYMBOL(its_map_vlpi);
EXPORT_SYMBOL(its_free_vcpu_irqs);
EXPORT_SYMBOL(arm64_is_fatal_ras_serror);
EXPORT_SYMBOL(aarch64_insn_gen_branch_reg);
EXPORT_SYMBOL(__icache_flags);
EXPORT_SYMBOL(its_unmap_vlpi);
EXPORT_SYMBOL(its_alloc_vcpu_irqs);
EXPORT_SYMBOL(idmap_t0sz);
EXPORT_SYMBOL(aarch64_insn_gen_logical_immediate);
EXPORT_SYMBOL(__hyp_reset_vectors);
EXPORT_SYMBOL(aarch64_insn_decode_register);
EXPORT_SYMBOL(its_get_vlpi);
EXPORT_SYMBOL(memblock_start_of_DRAM);
EXPORT_SYMBOL(aarch64_insn_gen_add_sub_imm);
EXPORT_SYMBOL(its_prop_update_vlpi);
EXPORT_SYMBOL(esr_get_class_string);
EXPORT_SYMBOL(__hyp_idmap_text_start);
EXPORT_SYMBOL(aarch64_insn_gen_extr);
EXPORT_SYMBOL(apei_claim_sea);
EXPORT_SYMBOL(its_invall_vpe);
EXPORT_SYMBOL(fpsimd_bind_state_to_cpu);
EXPORT_SYMBOL(arch_timer_get_kvm_info);
EXPORT_SYMBOL(__boot_cpu_mode);
EXPORT_SYMBOL(__hyp_idmap_text_end);
EXPORT_SYMBOL(spectre_bhb_patch_loop_iter);
EXPORT_SYMBOL(spectre_bhb_patch_clearbhb);
EXPORT_SYMBOL(arm64_get_meltdown_state);
EXPORT_SYMBOL(its_commit_vpe);
EXPORT_SYMBOL(gic_cpuif_has_vsgi);
EXPORT_SYMBOL(bp_hardening_data);
EXPORT_SYMBOL(dcache_clean_inval_poc);
EXPORT_SYMBOL(its_make_vpe_non_resident);
EXPORT_SYMBOL(arm64_get_spectre_v4_state);
EXPORT_SYMBOL(arm64_mismatched_32bit_el0);
EXPORT_SYMBOL(arm64_get_spectre_v2_state);
EXPORT_SYMBOL(icache_inval_pou);
EXPORT_SYMBOL(spectre_bhb_patch_loop_mitigation_enable);
EXPORT_SYMBOL(find_bug);
EXPORT_SYMBOL(irq_domain_activate_irq);
EXPORT_SYMBOL(irq_domain_deactivate_irq);
EXPORT_SYMBOL(spectre_bhb_patch_wa3);
EXPORT_SYMBOL(its_prop_update_vsgi);
extern struct exception_table_entry __start___kvm_ex_table;
extern struct exception_table_entry __stop___kvm_ex_table;
EXPORT_SYMBOL(__start___kvm_ex_table);
EXPORT_SYMBOL(__stop___kvm_ex_table);
EXPORT_SYMBOL(__hyp_reloc_begin);
EXPORT_SYMBOL(__hyp_reloc_end);
EXPORT_SYMBOL(bug_get_file_line);
EXPORT_SYMBOL(its_make_vpe_resident);

EXPORT_SYMBOL(arch_smp_send_reschedule);

EXPORT_SYMBOL(vl_info);

EXPORT_SYMBOL(get_arm64_ftr_reg);
EXPORT_SYMBOL(arm64_ftr_safe_value);

#ifdef CONFIG_ARM64_MTE
EXPORT_SYMBOL(mte_copy_tags_to_user);
EXPORT_SYMBOL(mte_copy_tags_from_user);
EXPORT_SYMBOL(mte_clear_page_tags);
#endif

EXPORT_SYMBOL(cpu_logical_map);

#ifdef CONFIG_VIRT_VTIMER_IRQ_BYPASS
EXPORT_SYMBOL(vtimer_mbigen_set_auto_clr);
EXPORT_SYMBOL(vtimer_mbigen_set_active);
EXPORT_SYMBOL(vtimer_mbigen_set_vector);
EXPORT_SYMBOL(vtimer_mbigen_get_active);
EXPORT_SYMBOL(vtimer_gic_set_auto_clr);

EXPORT_SYMBOL(vtimer_irqbypass);
#endif
#endif
