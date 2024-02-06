// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright(c) 2019 Huawei Technologies Co., Ltd
 */

#include <linux/acpi.h>
#include <linux/of.h>
#include <linux/init.h>
#include <linux/kvm_host.h>

#ifdef CONFIG_ACPI

/* ACPI Hisi oem table id str */
const char *oem_str[] = {
	"HIP06",	/* Hisi 1612 */
	"HIP07",	/* Hisi 1616 */
	"HIP08"		/* Hisi 1620 */
};

/*
 * Get Hisi oem table id.
 */
static void acpi_get_hw_cpu_type(void)
{
	struct acpi_table_header *table;
	acpi_status status;
	int i, str_size = ARRAY_SIZE(oem_str);

	/* Get oem table id from ACPI table header */
	status = acpi_get_table(ACPI_SIG_DSDT, 0, &table);
	if (ACPI_FAILURE(status)) {
		pr_err("Failed to get ACPI table: %s\n",
		       acpi_format_exception(status));
		return;
	}

	for (i = 0; i < str_size; ++i) {
		if (!strncmp(oem_str[i], table->oem_table_id, 5)) {
			hi_cpu_type = i;
			return;
		}
	}
}

#else
static void acpi_get_hw_cpu_type(void) {}
#endif

/* of Hisi cpu model str */
const char *of_model_str[] = {
	"Hi1612",
	"Hi1616"
};

static void of_get_hw_cpu_type(void)
{
	const char *cpu_type;
	int ret, i, str_size = ARRAY_SIZE(of_model_str);

	ret = of_property_read_string(of_root, "model", &cpu_type);
	if (ret < 0) {
		pr_err("Failed to get Hisi cpu model by OF.\n");
		return;
	}

	for (i = 0; i < str_size; ++i) {
		if (strstr(cpu_type, of_model_str[i])) {
			hi_cpu_type = i;
			return;
		}
	}
}

void probe_hisi_cpu_type(void)
{
	if (!acpi_disabled)
		acpi_get_hw_cpu_type();
	else
		of_get_hw_cpu_type();

	if (hi_cpu_type == UNKNOWN_HI_TYPE)
		pr_warn("UNKNOWN Hisi cpu type.\n");
}

#define NCSNP_MMIO_BASE	0x20107E238

/*
 * We have the fantastic HHA ncsnp capability on Kunpeng 920,
 * with which hypervisor doesn't need to perform a lot of cache
 * maintenance like before (in case the guest has non-cacheable
 * Stage-1 mappings).
 */
void probe_hisi_ncsnp_support(void)
{
	void __iomem *base;
	unsigned int high;

	kvm_ncsnp_support = false;

	if (hi_cpu_type != HI_1620)
		goto out;

	base = ioremap(NCSNP_MMIO_BASE, 4);
	if (!base) {
		pr_err("Unable to map MMIO region when probing ncsnp!\n");
		goto out;
	}

	high = readl_relaxed(base) >> 28;
	iounmap(base);
	if (high != 0x1)
		kvm_ncsnp_support = true;

out:
	kvm_info("Hisi ncsnp: %s\n", kvm_ncsnp_support ? "enabled" :
							 "disabled");
}
