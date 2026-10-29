// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Synopsys DesignWare Core I2C adapter driver.
 *
 * Copyright (C) 2024 Socionext Inc.
 * Portions Copyright (c) 2024 Synopsys, Inc. Used with permission. All rights reserved.
 *
 * This is a derivative work of Synopsys DesignWare I2C adapter driver.
 * - drivers/i2c/busses/i2c-designware-common.c
 *
 * Copyright (C) 2006 Texas Instruments.
 * Copyright (C) 2007 MontaVista Software Inc.
 * Copyright (C) 2009 Provigent Ltd.
 */
#include <linux/acpi.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <linux/units.h>

#include "core.h"

static char *trmnt_sources[] = {
	[TRMNT_7B_ADDR_NOACK_IDX] =
		"slave address not acknowledged (7bit mode)",
	[TRMNT_10ADDR1_NOACK_IDX] =
		"first address byte not acknowledged (10bit mode)",
	[TRMNT_10ADDR2_NOACK_IDX] =
		"second address byte not acknowledged (10bit mode)",
	[TRMNT_TXDATA_NOACK_IDX] =
		"data not acknowledged",
	[TRMNT_GCALL_NOACK_IDX] =
		"no acknowledgment for a general call",
	[TRMNT_GCALL_READ_IDX] =
		"read after general call",
	[TRMNT_HS_ACKDET] =
		"receive an erroneous acknowledgment for the Hs-mode controller code",
	[TRMNT_SBYTE_ACKDET_IDX] =
		"start byte acknowledged",
	[TRMNT_CTRLR_DIS_IDX] =
		"trying to use disabled adapter",
	[TRMNT_ARB_LOST_IDX] =
		"lost arbitration",
	[TRMNT_TGTFLUSH_TXFIFO_IDX] =
		"read command so flush old data in the TX FIFO",
	[TRMNT_TGT_ARBLOST_IDX] =
		"target lost the bus while transmitting data to a remote controller",
	[TRMNT_TGTRD_INTX_IDX] =
		"incorrect slave-transmitter mode configuration",
	[TRMNT_USER_TRMNT_IDX] =
		"terminate the transfer by the driver",
	[TRMNT_SDA_STUCK_AT_LOW_IDX] =
		"SDA is stuck at low",
};

static int dwc_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	struct dwc_i2c_dev *dev = context;

	*val = readl(dev->base + reg);

	return 0;
}

static int dwc_reg_write(void *context, unsigned int reg, unsigned int val)
{
	struct dwc_i2c_dev *dev = context;

	writel(val, dev->base + reg);

	return 0;
}

/**
 * dwc_i2c_init_regmap() - Initialize registers map
 * @dev: device private data
 *
 * Autodetects needed register access mode and creates the regmap with
 * corresponding read/write callbacks. This must be called before doing any
 * other register access.
 *
 * Return: 0 on success, otherwise a negative errno.
 */
static int dwc_i2c_init_regmap(struct dwc_i2c_dev *dev)
{
	struct regmap_config map_cfg = {
		.reg_bits = 32,
		.val_bits = 32,
		.reg_stride = 4,
		.disable_locking = true,
		.reg_read = dwc_reg_read,
		.reg_write = dwc_reg_write,
		.max_register = (dev->reg_size > 4) ? dev->reg_size - 4 : 0,
	};
	u32 reg;

	/*
	 * Skip detecting the registers map configuration if the regmap has
	 * already been provided by a higher code.
	 */
	if (dev->map)
		return 0;

	/*
	 * Note we'll check the return value of the regmap IO accessors only
	 * at the probe stage. The rest of the code won't do this because
	 * basically we have MMIO-based regmap, so none of the read/write
	 * methods can fail.
	 */
	dev->map = devm_regmap_init(dev->dev, NULL, dev, &map_cfg);
	if (IS_ERR(dev->map)) {
		dev_err(dev->dev, "Failed to init the registers map\n");
		return PTR_ERR(dev->map);
	}

	regmap_read(dev->map, DWC_IC_I2C_BLOCK_OFFSET(dev),
		    &dev->i2c_reg_offset);
	regmap_read(dev->map, DWC_IC_SMBUS_BLOCK_OFFSET(dev),
		    &dev->smbus_reg_offset);

	regmap_read(dev->map, DWC_IC_COMP_TYPE(dev), &reg);
	if (reg != DWC_IC_COMP_TYPE_VALUE) {
		dev_err(dev->dev, "Unknown Synopsys component type: 0x%08x\n",
			reg);
		return -ENODEV;
	}

	return 0;
}

static int dwc_i2c_validate_speed(struct dwc_i2c_dev *dev)
{
	u32 bus_freq = dev->timings.bus_freq_hz;

	switch (dev->caps.max_speed_mode) {
	case DWC_IC_CTRL_SPEED_HIGH:
		if (bus_freq == I2C_MAX_HIGH_SPEED_MODE_FREQ)
			return 0;
		fallthrough;
	case DWC_IC_CTRL_SPEED_FAST:
		if (bus_freq == I2C_MAX_FAST_MODE_PLUS_FREQ ||
		    bus_freq == I2C_MAX_FAST_MODE_FREQ)
			return 0;
		fallthrough;
	case DWC_IC_CTRL_SPEED_STD:
		if (bus_freq == I2C_MAX_STANDARD_MODE_FREQ)
			return 0;

		/* The case where max_speed is valid but bus_freq is not supported. */
		dev_err(dev->dev, "%s is unsupported (max speed mode: %s)\n",
			i2c_freq_mode_string(bus_freq),
			DWC_IC_SPEED_ID_TO_STR(dev->caps.max_speed_mode));
		return -EINVAL;
	default:
		dev_err(dev->dev, "Invalid max speed id: %d\n",
			dev->caps.max_speed_mode);
		return -EINVAL;
	}
}

#ifdef CONFIG_ACPI

#include <linux/dmi.h>

/*
 * The HCNT/LCNT information coming from ACPI should be the most accurate
 * for given platform. However, some systems get it wrong. On such systems
 * we get better results by calculating those based on the input clock.
 */
static const struct dmi_system_id dwc_i2c_no_acpi_params[] = {
	{},
};

static void dwc_i2c_acpi_params(struct device *device, char method[], u16 *hcnt,
				u16 *lcnt, u32 *sda_hold)
{
	struct acpi_buffer buf = { ACPI_ALLOCATE_BUFFER };
	acpi_handle handle = ACPI_HANDLE(device);
	union acpi_object *obj;
	u32 sda_tx_hold_cnt;

	if (dmi_check_system(dwc_i2c_no_acpi_params))
		return;

	if (ACPI_FAILURE(acpi_evaluate_object(handle, method, NULL, &buf)))
		return;

	obj = (union acpi_object *)buf.pointer;
	if (obj->type == ACPI_TYPE_PACKAGE && obj->package.count == 3) {
		const union acpi_object *objs = obj->package.elements;

		*hcnt = (u16)objs[0].integer.value;
		*lcnt = (u16)objs[1].integer.value;
		*sda_hold = (u32)objs[2].integer.value;
	}

	sda_tx_hold_cnt = (*sda_hold) & DWC_IC_SDA_HOLD_TX_MASK;
	if (sda_tx_hold_cnt > (*lcnt - 2)) {
		sda_tx_hold_cnt = *lcnt - 2;
		*sda_hold = ((*sda_hold) & DWC_IC_SDA_HOLD_RX_MASK) | sda_tx_hold_cnt;
	}

	kfree(buf.pointer);
}

static int dwc_i2c_acpi_configure(struct device *device)
{
	struct dwc_i2c_dev *dev = dev_get_drvdata(device);
	struct i2c_timings *t = &dev->timings;
	u32 ss_ht = 0, fp_ht = 0, hs_ht = 0, fs_ht = 0;
	u32 prop_icfreq = 0;

	/*
	 * Try to get SDA hold time and *CNT values from an ACPI method for
	 * selected speed modes.
	 */
	dwc_i2c_acpi_params(device, "SSCN", &dev->ss_hcnt, &dev->ss_lcnt,
			    &ss_ht);
	dwc_i2c_acpi_params(device, "FMCN", &dev->fs_hcnt, &dev->fs_lcnt,
			    &fs_ht);
	dwc_i2c_acpi_params(device, "FPCN", &dev->fp_hcnt, &dev->fp_lcnt,
			    &fp_ht);
	dwc_i2c_acpi_params(device, "HSCN", &dev->hs_hcnt, &dev->hs_lcnt,
			    &hs_ht);

	switch (t->bus_freq_hz) {
	case I2C_MAX_STANDARD_MODE_FREQ:
		dev->sda_hold_time = ss_ht;
		break;
	case I2C_MAX_FAST_MODE_PLUS_FREQ:
		dev->sda_hold_time = fp_ht;
		break;
	case I2C_MAX_HIGH_SPEED_MODE_FREQ:
		dev->sda_hold_time = hs_ht;
		break;
	case I2C_MAX_FAST_MODE_FREQ:
	default:
		dev->sda_hold_time = fs_ht;
		break;
	}

	device_property_read_u32(dev->dev, "snps,ic-clock-frequency", &prop_icfreq);
	dev->ic_clk_freq_khz = prop_icfreq;

	return 0;
}

static const u32 supported_speeds[] = {
	I2C_MAX_HIGH_SPEED_MODE_FREQ,
	I2C_MAX_FAST_MODE_PLUS_FREQ,
	I2C_MAX_FAST_MODE_FREQ,
	I2C_MAX_STANDARD_MODE_FREQ,
};

static u32 dwc_i2c_acpi_round_bus_speed(struct device *device)
{
	u32 acpi_speed;
	int i;

	acpi_speed = i2c_acpi_find_bus_speed(device);
	/*
	 * Some DSDTs use a non standard speed, round down to the lowest
	 * standard speed.
	 */
	for (i = 0; i < ARRAY_SIZE(supported_speeds); i++) {
		if (acpi_speed >= supported_speeds[i])
			return supported_speeds[i];
	}

	return 0;
}

#else /* CONFIG_ACPI */
static inline u32 dwc_i2c_acpi_round_bus_speed(struct device *device)
{
	return 0;
}
#endif /* CONFIG_ACPI */

static int dwc_i2c_adjust_bus_speed(struct dwc_i2c_dev *dev)
{
	u32 acpi_speed = dwc_i2c_acpi_round_bus_speed(dev->dev);
	struct i2c_timings *t = &dev->timings;
	u32 prop_freq = 0;

	device_property_read_u32(dev->dev, "clock-frequency", &prop_freq);

	if (acpi_speed && prop_freq)
		t->bus_freq_hz = min(prop_freq, acpi_speed);
	else if (acpi_speed)
		t->bus_freq_hz = acpi_speed;

	return 0;
}

u32 dwc_i2c_scl_hcnt(u32 ic_clk, u32 tSYMBOL, int offset)
{
	u32 duration = tSYMBOL;

	return DIV_ROUND_UP_ULL((u64)ic_clk * duration, MICRO) - 3 + offset;
}

u32 dwc_i2c_scl_lcnt(u32 ic_clk, u32 tLOW, int offset)
{
	u32 duration = tLOW;

	return DIV_ROUND_UP_ULL((u64)ic_clk * duration, MICRO) + offset;
}

int dwc_i2c_set_sda_hold(struct dwc_i2c_dev *dev)
{
	int ret;

	if (!dev->sda_hold_time) {
		/* Keep previous hold time setting if no one set it */
		ret = regmap_read(dev->map, DWC_IC_SDA_HOLD(dev),
				  &dev->sda_hold_time);
		if (ret)
			return ret;
	}

	/*
	 * Set RX hold time to the same value as TX hold time. This avoids TX
	 * arbitration lost in case an I2C slave pulls SDA down too quickly
	 * after the falling edge of SCL. If TX hold is zero, fall back to 1
	 * as a minimal non-zero RX hold.
	 */
	if (!(dev->sda_hold_time & DWC_IC_SDA_HOLD_RX_MASK)) {
		u32 tx_hold = FIELD_GET(DWC_IC_SDA_HOLD_TX_MASK,
					dev->sda_hold_time);

		dev->sda_hold_time |= FIELD_PREP(DWC_IC_SDA_HOLD_RX_MASK,
						  tx_hold ? tx_hold : 1);
	}

	dev_dbg(dev->dev, "SDA Hold Time TX:RX = %lu:%lu\n",
		FIELD_GET(DWC_IC_SDA_HOLD_TX_MASK, dev->sda_hold_time),
		FIELD_GET(DWC_IC_SDA_HOLD_RX_MASK, dev->sda_hold_time));

	return 0;
}

int dwc_i2c_set_sda_setup(struct dwc_i2c_dev *dev)
{
	u32 bus_freq = dev->timings.bus_freq_hz;
	u32 ic_clk;
	u32 data_setup_ns;
	u32 sda_setup;
	int ret;

	if (bus_freq == I2C_MAX_STANDARD_MODE_FREQ)
		data_setup_ns = 250;
	else if (bus_freq == I2C_MAX_FAST_MODE_PLUS_FREQ)
		data_setup_ns = 50;
	else
		data_setup_ns = 100;

	ic_clk = dwc_i2c_clk_rate(dev);
	if (!ic_clk) {
		ret = regmap_read(dev->map, DWC_IC_SDA_SETUP(dev),
				  &dev->sda_setup_time);
		if (ret)
			return ret;

		dev->sda_setup_time &= DWC_IC_SDA_SETUP_MASK;
		return 0;
	}

	sda_setup = DIV_ROUND_UP_ULL((u64)ic_clk * data_setup_ns, MICRO);
	dev->sda_setup_time = min_t(u32, sda_setup - 1, DWC_IC_SDA_SETUP_MASK);

	return 0;
}

int dwc_i2c_terminate_by_polling(struct dwc_i2c_dev *dev)
{
	unsigned int enable;
	int ret;

	/* Make sure the interrupts are disabled. */
	if (WARN_ON(!__dwc_i2c_interrupts_masked(dev)))
		return -EINVAL;

	regmap_read(dev->map, DWC_IC_ENABLE(dev), &enable);
	regmap_write(dev->map, DWC_IC_ENABLE(dev),
		     enable | DWC_IC_ENABLE_TERMINATE);
	ret = regmap_read_poll_timeout(dev->map, DWC_IC_ENABLE(dev), enable,
				       !(enable & DWC_IC_ENABLE_TERMINATE), 10,
				       100);
	dev->status &= ~STATUS_REQ_TERMINATE;
	if (ret)
		dev_err(dev->dev,
			"timeout while trying to terminate current transfer\n");

	return ret;
}

void __dwc_i2c_disable(struct dwc_i2c_dev *dev)
{
	unsigned int raw_intr_stats;
	int timeout = 100;
	bool terminate_needed;
	unsigned int status;

	regmap_read(dev->map, DWC_IC_INTR_RAW_STAT(dev), &raw_intr_stats);
	terminate_needed = raw_intr_stats & DWC_IC_INTR_CTRLR_ON_HOLD_MASK;
	if (terminate_needed)
		dwc_i2c_terminate_by_polling(dev);

	do {
		__dwc_i2c_disable_nowait(dev);
		/*
		 * The enable status register may be unimplemented, but
		 * in that case this test reads zero and exits the loop.
		 */
		regmap_read(dev->map, DWC_IC_ENABLE_STATUS(dev), &status);
		if ((status & DWC_IC_ENABLE_STATUS_IC_EN_MASK) == 0)
			return;

		/*
		 * Wait 10 times the signaling period of the highest I2C
		 * transfer supported by the driver (for 400KHz this is
		 * 25us) as described in the DesignWare Core I2C databook.
		 */
		usleep_range(25, 250);
	} while (timeout--);

	dev_warn(dev->dev, "timeout in disabling adapter\n");
}

void dwc_i2c_do_reset(struct dwc_i2c_dev *dev)
{
	int ret;
	unsigned int val;

	regmap_write(dev->map, DWC_IC_RESET_CTRL(dev),
		     DWC_IC_RESET_CTRL_SOFT_RESET);
	ret = regmap_read_poll_timeout(dev->map, DWC_IC_RESET_CTRL(dev), val,
				       !(val & DWC_IC_RESET_CTRL_SOFT_RESET),
				       10, 1000);

	if (ret)
		dev_err(dev->dev, "timemout in applying the soft reset\n");
}

u32 dwc_i2c_clk_rate(struct dwc_i2c_dev *dev)
{
	/*
	 * Clock is not necessary if we got LCNT/HCNT values directly from
	 * the platform code.
	 */
	if (!dev->get_clk_rate_khz) {
		if (dev->ic_clk_freq_khz)
			return dev->ic_clk_freq_khz;
		else
			return 0;
	}
	return dev->get_clk_rate_khz(dev);
}

int dwc_i2c_prepare_clk(struct dwc_i2c_dev *dev, bool prepare)
{
	int ret;

	if (prepare) {
		/* Optional interface clock */
		ret = clk_prepare_enable(dev->pclk);
		if (ret)
			return ret;

		ret = clk_prepare_enable(dev->clk);
		if (ret)
			clk_disable_unprepare(dev->pclk);

		return ret;
	}

	clk_disable_unprepare(dev->clk);
	clk_disable_unprepare(dev->pclk);

	return 0;
}
EXPORT_SYMBOL_GPL(dwc_i2c_prepare_clk);

/*
 * Waiting for bus not busy
 */
int dwc_i2c_wait_bus_not_busy(struct dwc_i2c_dev *dev)
{
	unsigned int status;
	int ret;

	ret = regmap_read_poll_timeout(dev->map, DWC_IC_STATUS(dev), status,
				       !(status & DWC_IC_STATUS_ACTIVITY_MASK),
				       1100, 20000);
	if (ret)
		dev_err(dev->dev, "timeout waiting for bus ready\n");

	return ret;
}

int dwc_i2c_handle_tx_terminate(struct dwc_i2c_dev *dev)
{
	unsigned long trmnt_source = dev->trmnt_source;
	int i;

	if (trmnt_source & IC_TX_TRMNT_SOURCE_NOACK_MASK) {
		for_each_set_bit(i, &trmnt_source, ARRAY_SIZE(trmnt_sources))
			dev_dbg(dev->dev, "%s: %s\n", __func__,
				trmnt_sources[i]);
		return -EREMOTEIO;
	}

	for_each_set_bit(i, &trmnt_source, ARRAY_SIZE(trmnt_sources))
		dev_err(dev->dev, "%s: %s\n", __func__, trmnt_sources[i]);

	if (trmnt_source & DWC_IC_TX_TRMNT_ARB_LOST_MASK)
		return -EAGAIN;
	else if (trmnt_source & DWC_IC_TX_TRMNT_GCALL_READ_MASK)
		return -EINVAL; /* wrong msgs[] data */
	else
		return -EIO;
}

u32 dwc_i2c_func(struct i2c_adapter *adap)
{
	struct dwc_i2c_dev *dev = i2c_get_adapdata(adap);

	return dev->functionality;
}

void dwc_i2c_disable(struct dwc_i2c_dev *dev)
{
	/* Disable controller */
	__dwc_i2c_disable(dev);

	/* Disable all interrupts */
	__dwc_i2c_write_intr_mask(dev, 0);
	__dwc_i2c_write_smbus_intr_mask(dev, 0);
	regmap_write(dev->map, DWC_IC_INTR_CLR(dev), DWC_IC_INTR_CLR_INTR_MASK);
	if (dev->caps.smbus)
		regmap_write(dev->map, DWC_IC_SMBUS_INTR_CLR(dev),
			     DWC_IC_SMBUS_INTR_CLR_ALL);
}
EXPORT_SYMBOL_GPL(dwc_i2c_disable);

void dwc_i2c_disable_int(struct dwc_i2c_dev *dev)
{
	__dwc_i2c_write_intr_mask(dev, 0);
	__dwc_i2c_write_smbus_intr_mask(dev, 0);
}

static int dwc_i2c_get_capabilities(struct dwc_i2c_dev *dev)
{
	u32 caps;

	regmap_read(dev->map, DWC_IC_CAPABILITIES(dev), &caps);
	dev->caps.smbus = caps & DWC_IC_CAPABILITIES_SMBUS;
	dev->caps.max_speed_mode = dev->caps.smbus ? DWC_IC_CTRL_SPEED_FAST :
						     DWC_IC_CTRL_SPEED_HIGH;

	regmap_read(dev->map, DWC_IC_I2C_CAPABILITIES(dev), &caps);
	dev->caps.supports_bus_clear =
		caps & DWC_IC_I2C_CAPABILITIES_BUS_CLEAR_FEATURE;
	dev->caps.supports_10bit_addr_ctrlr =
		caps & DWC_IC_I2C_CAPABILITIES_10BITADDR_CTRLR;
	if (caps & DWC_IC_I2C_CAPABILITIES_ULTRA_FAST_MODE) {
		dev_err(dev->dev,
			"the driver does not support the Ultra Fast-mode\n");
		return -EINVAL;
	}

	if (!dev->caps.ctrlr_holds_bus_with_tx_fifo_empty) {
		const char *prop = "snps,ctrlr-holds-bus-with-tx-fifo-empty";

		dev->caps.ctrlr_holds_bus_with_tx_fifo_empty =
			device_property_read_bool(dev->dev, prop);
	}

	return 0;
}

static int dwc_i2c_detect_smbus_mode(struct dwc_i2c_dev *dev)
{
	dev->is_smbus_mode = dev->is_smbus_mode ||
			     device_property_read_bool(dev->dev, "smbus");

	if (dev->is_smbus_mode && !dev->caps.smbus) {
		dev_err(dev->dev,
			"smbus mode is specified, but the device has no smbus capability\n");
		return -EINVAL;
	}

	if (dev->is_smbus_mode)
		dev_dbg(dev->dev, "operates as an smbus\n");

	return 0;
}

static int dwc_i2c_get_timings(struct dwc_i2c_dev *dev)
{
	int ret;
	struct i2c_timings *t = &dev->timings;

	i2c_parse_fw_timings(dev->dev, t, true);

	ret = dwc_i2c_adjust_bus_speed(dev);
	if (ret)
		return ret;

	ret = dwc_i2c_validate_speed(dev);
	if (ret)
		return ret;

	if (has_acpi_companion(dev->dev))
		dwc_i2c_acpi_configure(dev->dev);

	if (!dev->sda_hold_time && t->sda_hold_ns) {
		u64 clk_khz = dwc_i2c_clk_rate(dev);
		u64 ht = clk_khz * t->sda_hold_ns;

		dev->sda_hold_time = DIV_U64_ROUND_CLOSEST(ht, MICRO);
	}

	return 0;
}

static void dwc_i2c_set_fifo_size(struct dwc_i2c_dev *dev)
{
	if (!dev->tx_fifo_depth)
		device_property_read_u32(dev->dev, "snps,tx-fifo-depth",
					 &dev->tx_fifo_depth);

	dev->tx_fifo_depth = clamp_val(dev->tx_fifo_depth,
				       DWC_MIN_TX_FIFO_DEPTH,
				       DWC_MAX_TX_FIFO_DEPTH);

	if (!dev->rx_fifo_depth)
		device_property_read_u32(dev->dev, "snps,rx-fifo-depth",
					 &dev->rx_fifo_depth);

	dev->rx_fifo_depth = clamp_val(dev->rx_fifo_depth,
				       DWC_MIN_RX_FIFO_DEPTH,
				       DWC_MAX_RX_FIFO_DEPTH);
}

static void dwc_i2c_configure(struct dwc_i2c_dev *dev)
{
	if (i2c_detect_slave_mode(dev->dev))
		dwc_i2c_configure_target(dev);
	else
		dwc_i2c_configure_controller(dev);
}

int dwc_i2c_probe(struct dwc_i2c_dev *dev)
{
	int ret;

	device_set_node(&dev->adapter.dev, dev_fwnode(dev->dev));

	ret = dwc_i2c_init_regmap(dev);
	if (ret)
		return ret;

	ret = dwc_i2c_get_capabilities(dev);
	if (ret)
		return ret;

	ret = dwc_i2c_detect_smbus_mode(dev);
	if (ret)
		return ret;

	ret = dwc_i2c_get_timings(dev);
	if (ret)
		return ret;

	dwc_i2c_set_fifo_size(dev);

	dwc_i2c_configure(dev);

	switch (dev->mode) {
	case DWC_IC_CTRL_OP_MODE_TARGET:
		return dwc_i2c_probe_target(dev);
	case DWC_IC_CTRL_OP_MODE_CONTROLLER:
		return dwc_i2c_probe_controller(dev);
	default:
		dev_err(dev->dev, "Wrong operation mode: %d\n", dev->mode);
		return -EINVAL;
	}
}
EXPORT_SYMBOL_GPL(dwc_i2c_probe);

MODULE_DESCRIPTION("Synopsys DesignWare Core I2C bus adapter core");
MODULE_LICENSE("GPL");
