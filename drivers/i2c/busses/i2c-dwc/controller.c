// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Synopsys DesignWare Core I2C adapter driver (controller mode).
 *
 * Copyright (C) 2024 Socionext Inc.
 * Portions Copyright (c) 2024 Synopsys, Inc. Used with permission. All rights reserved.
 *
 * This is a derivative work of Synopsys DesignWare I2C adapter driver.
 * - drivers/i2c/busses/i2c-designware-master.c
 *
 * Copyright (C) 2006 Texas Instruments.
 * Copyright (C) 2007 MontaVista Software Inc.
 * Copyright (C) 2009 Provigent Ltd.
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/spinlock.h>
#include <linux/units.h>
#include <linux/time64.h>

#include "core.h"

static u32
dwc_i2c_calculate_scl_padding(u32 fscl, u32 thigh, u32 tlow, u32 trcl, u32 tfcl)
{
	u32 scl_min_ns = DIV_ROUND_UP(NSEC_PER_SEC, fscl);
	u32 scl_ns = trcl + thigh + tfcl + tlow;

	return scl_min_ns > scl_ns ? scl_min_ns - scl_ns : 0;
}

static int dwc_i2c_set_timings_controller(struct dwc_i2c_dev *dev)
{
	struct i2c_timings *t = &dev->timings;
	u32 ic_clk, padding;
	int ret;

	if (!dev->spklen_cnt) {
		u32 tsp = min_t(u32, t->digital_filter_width_ns, 50);

		ic_clk = dev->clk ? dwc_i2c_clk_rate(dev) : 0;
		dev->spklen_cnt = DIV_ROUND_UP_ULL((u64)ic_clk * tsp, MICRO) ?:
									       1;
	}
	dev_dbg(dev->dev, "tSP CNT (SS, FS, FS+) = %u\n", dev->spklen_cnt);

	/* Calculate SCL timing parameters for standard mode if not set */
	if (!dev->ss_hcnt || !dev->ss_lcnt) {
		ic_clk = dwc_i2c_clk_rate(dev);
		padding = dwc_i2c_calculate_scl_padding(
				I2C_MAX_STANDARD_MODE_FREQ, 4000, 4700,
				t->scl_rise_ns, t->scl_fall_ns);
		dev->ss_hcnt = dwc_i2c_scl_hcnt(
			ic_clk, 4000, /* tHD;STA = tHIGH = 4.0 us */
			t->sda_fall_ns,
			padding,
			-dev->spklen_cnt);
		dev->ss_lcnt =
			dwc_i2c_scl_lcnt(ic_clk, 4700, /* tLOW = 4.7 us */
					 t->scl_fall_ns, 0, 0, 0); /* No offset */
	}
	dev_dbg(dev->dev, "Standard Mode HCNT:LCNT = %d:%d\n", dev->ss_hcnt,
		dev->ss_lcnt);

	/* Calculate SCL timing parameters for fast mode if not set. */
	if (!dev->fs_hcnt || !dev->fs_lcnt) {
		ic_clk = dwc_i2c_clk_rate(dev);
		padding = dwc_i2c_calculate_scl_padding(
				I2C_MAX_FAST_MODE_FREQ, 600, 1300,
				t->scl_rise_ns, t->scl_fall_ns);
		dev->fs_hcnt = dwc_i2c_scl_hcnt(
			ic_clk, 600, /* tHD;STA = tHIGH = 0.6 us */
			t->sda_fall_ns,
			padding,
			-dev->spklen_cnt);
		dev->fs_lcnt =
			dwc_i2c_scl_lcnt(ic_clk, 1300, /* tLOW = 1.3 us */
					 t->scl_fall_ns, 0, 0, 0); /* No offset */
	}
	dev_dbg(dev->dev, "Fast Mode HCNT:LCNT = %d:%d\n", dev->fs_hcnt,
		dev->fs_lcnt);

	/* Calculate SCL timing parameters for fast mode plus if not set. */
	if (!dev->fp_hcnt || !dev->fp_lcnt) {
		ic_clk = dwc_i2c_clk_rate(dev);
		padding = dwc_i2c_calculate_scl_padding(
				I2C_MAX_FAST_MODE_PLUS_FREQ, 260, 500,
				t->scl_rise_ns, t->scl_fall_ns);
		dev->fp_hcnt = dwc_i2c_scl_hcnt(ic_clk,
						260, /* tHD;STA = tHIGH = 260 ns */
						t->sda_fall_ns,
						padding,
						-dev->spklen_cnt);
		dev->fp_lcnt = dwc_i2c_scl_lcnt(ic_clk, 500, /* tLOW = 500 ns */
						t->scl_fall_ns,
						0,
						0,
						0); /* No offset */
	}
	dev_dbg(dev->dev, "Fast Mode Plus HCNT:LCNT = %d:%d\n", dev->fp_hcnt,
		dev->fp_lcnt);

	/* Calculate SCL timing parameters for high speed mode if available. */
	if (dev->caps.max_speed_mode == DWC_IC_CTRL_SPEED_HIGH) {
		if (!dev->hs_spklen_cnt) {
			u32 tsp = min_t(u32, t->digital_filter_width_ns, 10);

			ic_clk = dev->clk ? dwc_i2c_clk_rate(dev) : 0;
			padding = dwc_i2c_calculate_scl_padding(
					I2C_MAX_HIGH_SPEED_MODE_FREQ, 60, 160,
					t->scl_rise_ns, t->scl_fall_ns);
			dev->hs_spklen_cnt =
				DIV_ROUND_UP_ULL((u64)ic_clk * tsp, MICRO) ?: 1;
		}
		dev_dbg(dev->dev, "tSP CNT (HS) = %u\n", dev->hs_spklen_cnt);

		if (!dev->hs_hcnt || !dev->hs_lcnt) {
			ic_clk = dwc_i2c_clk_rate(dev);
			dev->hs_hcnt = dwc_i2c_scl_hcnt(ic_clk,
							60, /* tHIGH = 60 ns */
							0,
							0,
							-dev->hs_spklen_cnt);
			dev->hs_lcnt = dwc_i2c_scl_lcnt(ic_clk,
							160, /* tHD;STA = tLOW = 160 ns */
							t->scl_fall_ns,
							t->sda_fall_ns,
							padding,
							0); /* No offset */
		}
		dev_dbg(dev->dev, "High Speed Mode HCNT:LCNT = %d:%d\n",
			dev->hs_hcnt, dev->hs_lcnt);
	}

	ret = dwc_i2c_set_sda_hold(dev);
	if (ret)
		return ret;

	dev_dbg(dev->dev, "Bus speed: %s\n",
		i2c_freq_mode_string(t->bus_freq_hz));
	return 0;
}

/**
 * dwc_i2c_init_controller() - Initialize the DWC I2C controller hardware
 * @dev: device private data
 *
 * This functions configures and enables the I2C controller.
 * This function is called during I2C init function, and in case of timeout at
 * run time.
 *
 * Return: 0 on success, or negative errno otherwise.
 */
static int dwc_i2c_init_controller(struct dwc_i2c_dev *dev)
{
	u32 hcnt, lcnt;
	u32 bus_freq = dev->timings.bus_freq_hz;

	/* Disable the adapter */
	__dwc_i2c_disable(dev);

	/* Write speed timing parameters (standard (default), fast, fast+) */
	if (bus_freq == I2C_MAX_FAST_MODE_FREQ) {
		hcnt = dev->fs_hcnt;
		lcnt = dev->fs_lcnt;
	} else if (bus_freq == I2C_MAX_FAST_MODE_PLUS_FREQ) {
		hcnt = dev->fp_hcnt;
		lcnt = dev->fp_lcnt;
	} else {
		hcnt = dev->ss_hcnt;
		lcnt = dev->ss_lcnt;
	}
	regmap_write(dev->map, DWC_IC_SCL_HCNT(dev), hcnt);
	regmap_write(dev->map, DWC_IC_SCL_LCNT(dev), lcnt);

	/* Write high speed timing parameters if supported */
	if (dev->caps.max_speed_mode == DWC_IC_CTRL_SPEED_HIGH) {
		regmap_write(dev->map, DWC_IC_HS_SCL_HCNT(dev), dev->hs_hcnt);
		regmap_write(dev->map, DWC_IC_HS_SCL_LCNT(dev), dev->hs_lcnt);
	}

	/* Write SDA hold time */
	regmap_write(dev->map, DWC_IC_SDA_HOLD(dev), dev->sda_hold_time);

	/* write the spike length to be suppressed */
	regmap_write(dev->map, DWC_IC_SPKLEN(dev), dev->spklen_cnt);
	if (dev->caps.max_speed_mode == DWC_IC_CTRL_SPEED_HIGH)
		regmap_write(dev->map, DWC_IC_HS_SPKLEN(dev),
			     dev->hs_spklen_cnt);

	if (dev->caps.supports_bus_clear) {
		unsigned int scl_low, scl_low_max, sda_low;
		/*
		 * Configure the duration for SDA and SCL stuck at low timeout.
		 * The I2C specification does not place limits on the maximum
		 * duration of stuck at low time, so write the maximum value for
		 * this register.
		 */
		if (dev->is_smbus_mode) {
			u64 clk_khz = dwc_i2c_clk_rate(dev);

			scl_low = clk_khz * SMBCLK_LOW_TIMEOUT_MS;
			scl_low_max = clk_khz * SMBCLK_LOW_TIMEOUT_MAX_MS;
			/*
			 * Use tTIMEOUT,MAX as the SMBDAT timeout according
			 * to the SMBus specification's recommendation.
			 */
			sda_low = scl_low_max;
		} else {
			scl_low = 0xffffffff;
			scl_low_max = 0xffffffff;
			sda_low = 0xffffffff;
		}
		regmap_write(dev->map, DWC_IC_SCL_STUCK_AT_LOW_TIMEOUT(dev),
			     scl_low);
		regmap_write(dev->map, DWC_IC_SCL_STUCK_AT_LOW_TIMEOUT_MAX(dev),
			     scl_low_max);
		regmap_write(dev->map, DWC_IC_SDA_STUCK_AT_LOW_TIMEOUT(dev),
			     sda_low);
		dev_dbg(dev->dev, "scl low timeout min: %u\n", scl_low);
		dev_dbg(dev->dev, "scl low timeout max: %u\n", scl_low_max);
		dev_dbg(dev->dev, "sda low timeout    : %u\n", sda_low);
	}

	/* Set the tLOW:TEXT, tLOW:CEXT and tHIGH,MAX parameters */
	if (dev->is_smbus_mode) {
		u32 clk_khz = dwc_i2c_clk_rate(dev);
		unsigned int text = clk_khz * SMBUS_LOW_TEXT_MS;
		unsigned int cext = clk_khz * SMBUS_LOW_CEXT_MS;
		u16 idle_cnt = DIV_ROUND_UP_ULL(
			(u64)clk_khz * SMBUS_THIGH_MAX_US, MILLI);

		regmap_write(dev->map, DWC_IC_SMBUS_CLK_LOW_TEXT(dev), text);
		regmap_write(dev->map, DWC_IC_SMBUS_CLK_LOW_CEXT(dev), cext);
		regmap_write(dev->map, DWC_IC_SMBUS_THIGH_MAX_IDLE_COUNT(dev),
			     idle_cnt);
	}

	/* Configure Tx/Rx FIFO threshold levels */
	regmap_write(dev->map, DWC_IC_TX_TL(dev), dev->tx_fifo_depth / 2);
	regmap_write(dev->map, DWC_IC_RX_TL(dev), 0);

	/* Configure the I2C controller */
	regmap_write(dev->map, DWC_IC_CTRL(dev), dev->controller_cfg);

	return 0;
}

static int dwc_i2c_xfer_init(struct dwc_i2c_dev *dev)
{
	struct i2c_msg *msgs = dev->msgs;
	u32 ic_ctrl = 0, ic_tar = 0;
	unsigned int intr_mask, smbus_intr_mask;
	unsigned long flags;

	/* Disable the adapter */
	__dwc_i2c_disable(dev);

	/* If the slave address is ten bit address, enable 10BITADDR */
	if (msgs[dev->msg_write_idx].flags & I2C_M_TEN) {
		if (!dev->caps.supports_10bit_addr_ctrlr) {
			dev_err(dev->dev, "10-bit address is unavailable\n");
			return -EINVAL;
		}

		ic_ctrl = DWC_IC_CTRL_10BITADDR_CTRLR;
		/*
		 * If I2C_DYNAMIC_TAR_UPDATE is set, the 10-bit addressing
		 * mode has to be enabled via bit 12 of IC_TAR register.
		 * We set it always as I2C_DYNAMIC_TAR_UPDATE can't be
		 * detected from registers.
		 */
		ic_tar = DWC_IC_TAR_10BITADDR_CTRLR;
	}

	regmap_update_bits(dev->map, DWC_IC_CTRL(dev),
			   DWC_IC_CTRL_10BITADDR_CTRLR, ic_ctrl);

	if (dev->are_msgs_smbus_quick_cmd)
		ic_tar |= DWC_IC_TAR_SMBUS_QUICK_CMD | DWC_IC_TAR_SPECIAL;

	/*
	 * Set the slave (target) address and enable 10-bit addressing mode
	 * if applicable.
	 */
	regmap_write(dev->map, DWC_IC_TAR(dev),
		     msgs[dev->msg_write_idx].addr | ic_tar);

	/* Enforce disabled interrupts (due to HW issues) */
	__dwc_i2c_write_intr_mask(dev, 0);
	__dwc_i2c_write_smbus_intr_mask(dev, 0);

	/* Enable the adapter */
	__dwc_i2c_enable(dev);

	/* Clear interrupts */
	regmap_write(dev->map, DWC_IC_INTR_CLR(dev), DWC_IC_INTR_CLR_INTR_MASK);
	if (dev->caps.smbus)
		regmap_write(dev->map, DWC_IC_SMBUS_INTR_CLR(dev),
			     DWC_IC_SMBUS_INTR_CLR_ALL);

	/* Enable interrupts */
	intr_mask = DWC_IC_INTR_CONTROLLER_MASK;
	smbus_intr_mask = 0;
	if (dev->is_smbus_mode) {
		intr_mask |= DWC_IC_INTR_SCL_STUCK_DET_MASK;
		smbus_intr_mask |= (DWC_IC_SMBUS_INTR_TEXT_TIMEOUT_MASK |
				    DWC_IC_SMBUS_INTR_CEXT_TIMEOUT_MASK);
	}

	spin_lock_irqsave(&dev->lock, flags);
	__dwc_i2c_write_intr_mask(dev, intr_mask);
	__dwc_i2c_write_smbus_intr_mask(dev, smbus_intr_mask);
	spin_unlock_irqrestore(&dev->lock, flags);

	return 0;
}

/*
 * Initiate (and continue) low level controller read/write transaction.
 * This function is only called from dwc_i2c_isr, and pumping i2c_msg
 * messages into the tx buffer.  Even if the size of i2c_msg data is
 * longer than the size of the tx buffer, it handles everything.
 */
static void dwc_i2c_xfer_msg(struct dwc_i2c_dev *dev)
{
	u32 intr_mask;
	u32 addr = dev->msgs[dev->msg_write_idx].addr;

	__dwc_i2c_read_intr_mask(dev, &intr_mask);

	for (; dev->msg_write_idx < dev->msgs_num; dev->msg_write_idx++) {
		struct i2c_msg *msg = &dev->msgs[dev->msg_write_idx];
		int tx_limit, rx_limit;
		unsigned int flr;
		bool is_recv_len_byte = (msg->flags & I2C_M_RECV_LEN) &&
					dev->buf_write_idx == 0;

		/*
		 * If target address has changed, we need to
		 * reprogram the target address in the I2C
		 * adapter when we are done with this transfer.
		 */
		if (msg->addr != addr) {
			dev_err(dev->dev, "%s: invalid target address\n",
				__func__);
			dev->msg_err = -EINVAL;
			goto update_intr_mask;
		}

		regmap_read(dev->map, DWC_IC_TXFLR(dev), &flr);
		tx_limit = dev->tx_fifo_depth - flr;

		regmap_read(dev->map, DWC_IC_RXFLR(dev), &flr);
		rx_limit = dev->rx_fifo_depth - flr;

		for (; dev->buf_write_idx < msg->len; dev->buf_write_idx++) {
			u32 cmd = 0;

			/* Need to wait the read length data.*/
			if (is_recv_len_byte && dev->buf_write_idx != 0)
				break;

			/* Avoid filling the tx buffer before TERMINATE. */
			if (dev->status & STATUS_REQ_TERMINATE) {
				intr_mask &= ~DWC_IC_INTR_TX_EMPTY_MASK;
				break;
			}

			if (tx_limit <= 0 || rx_limit <= 0)
				break;

			/*
			 * If IC_EMPTYFIFO_HOLD_CTRLR_EN is set we must
			 * manually set the stop bit. However, it cannot be
			 * detected from the registers so we set it always
			 * when writing/reading the last byte.
			 *
			 * Note that i2c-core always sets the buffer length of
			 * I2C_FUNC_SMBUS_BLOCK_DATA to 1. The length will
			 * be adjusted when receiving the first byte.
			 * Thus we can't stop the transaction here.
			 */
			if (dev->msg_write_idx == dev->msgs_num - 1 &&
			    dev->buf_write_idx == msg->len - 1 &&
			    !is_recv_len_byte)
				cmd |= DWC_IC_DATA_CMD_STOP;

			/*
			 * If IC_EMPTYFIFO_HOLD_CTRLR_EN is set, we must
			 * manually set restart bit between messages.
			 */
			if (dev->msg_write_idx > 0 && dev->buf_write_idx == 0)
				cmd |= DWC_IC_DATA_CMD_RESTART;

			if (dev->are_msgs_smbus_quick_cmd) {
				/*
				 * In SMBus Quick Command protocol, only a byte
				 * that has an information of 0/1 in R/W field
				 * is sent, and there is no data to be received.
				 */
				if (msg->flags & I2C_M_RD)
					cmd |= DWC_IC_DATA_CMD_READ_CMD;
			} else if (msg->flags & I2C_M_RD) {
				/* Avoid rx buffer overrun */
				if (dev->rx_outstanding >= dev->rx_fifo_depth)
					break;

				cmd |= DWC_IC_DATA_CMD_READ_CMD;
				rx_limit--;
				dev->rx_outstanding++;
			} else {
				cmd |= msg->buf[dev->buf_write_idx];
			}

			regmap_write(dev->map, DWC_IC_DATA_CMD(dev), cmd);
			tx_limit--;
		}

		/* more bytes to be written */
		if (dev->buf_write_idx < msg->len || is_recv_len_byte) {
			/*
			 * Because we don't know the buffer length in the
			 * I2C_FUNC_SMBUS_BLOCK_DATA case, we can't stop the
			 * transaction here. Also disable the TX_EMPTY IRQ
			 * while waiting for the data length byte not to
			 * progress the following write operations.
			 */
			if (is_recv_len_byte)
				intr_mask &= ~DWC_IC_INTR_TX_EMPTY_MASK;

			goto update_intr_mask;
		}

		/* complete the write for msgs[dev->msg_write_idx] */
		dev->buf_write_idx = 0;
	}

update_intr_mask:
	/*
	 * If i2c_msg index search is completed, we don't need TX_EMPTY
	 * interrupt any more.
	 */
	if (dev->msg_write_idx == dev->msgs_num)
		intr_mask &= ~DWC_IC_INTR_TX_EMPTY_MASK;

	if (dev->msg_err)
		intr_mask = 0;

	__dwc_i2c_write_intr_mask(dev, intr_mask);
}

static void dwc_i2c_read(struct dwc_i2c_dev *dev)
{
	for (; dev->msg_read_idx < dev->msgs_num; dev->msg_read_idx++) {
		struct i2c_msg *msg = &dev->msgs[dev->msg_read_idx];
		unsigned int rx_valid;

		if (!(msg->flags & I2C_M_RD))
			continue;

		regmap_read(dev->map, DWC_IC_RXFLR(dev), &rx_valid);

		for (; dev->buf_read_idx < msg->len; dev->buf_read_idx++) {
			unsigned int tmp;

			if (rx_valid == 0)
				break;

			regmap_read(dev->map, DWC_IC_DATA_CMD(dev), &tmp);
			tmp &= DWC_IC_DATA_CMD_DAT_MASK;
			/* Ensure length byte is a valid value */
			if ((msg->flags & I2C_M_RECV_LEN) &&
			    dev->buf_read_idx == 0) {
				unsigned int intr_mask;

				if (tmp > I2C_SMBUS_BLOCK_MAX) {
					dev_err(dev->dev,
						"Invalid read length: %u\n",
						tmp);
					dev->status |= STATUS_REQ_TERMINATE;
					dev->msg_err = -EINVAL;
				}

				msg->len += tmp;

				/*
				 * TERMINAT is required to issue the STOP
				 * condition if the read byte is the last byte
				 * of the messages. In addition, advancing write
				 * msg/buf indices is also required to mark the
				 * warite operation has been finished.
				 */
				if (dev->msg_write_idx == dev->msgs_num - 1 &&
				    dev->buf_write_idx >= msg->len) {
					dev->buf_write_idx = 0;
					dev->msg_write_idx++;
					dev->status |= STATUS_REQ_TERMINATE;
				}

				/*
				 * Received buffer length, re-enable TX_EMPTY
				 * interrupt to resume the SMBUS transaction.
				 */
				__dwc_i2c_read_intr_mask(dev, &intr_mask);
				intr_mask |= DWC_IC_INTR_TX_EMPTY_MASK;
				__dwc_i2c_write_intr_mask(dev, intr_mask);
			}
			msg->buf[dev->buf_read_idx] = tmp;
			dev->rx_outstanding--;
			rx_valid--;
		}

		/* more bytes to be read */
		if (dev->buf_read_idx < msg->len)
			return;

		/* complete the read for msgs[dev->msg_read_idx] */
		dev->buf_read_idx = 0;
	}
}

static u32 dwc_i2c_read_clear_intrbits(struct dwc_i2c_dev *dev)
{
	unsigned int stat;
	u32 clear_bits = 0;

	/*
	 * The IC_INTR_STAT register just indicates "enabled" interrupts.
	 * The unmasked raw version of interrupt status bits is available
	 * in the IC_INTR_RAW_STAT register.
	 *
	 * That is,
	 *   stat = readl(IC_INTR_STAT);
	 * equals to,
	 *   stat = readl(IC_INTR_RAW_STAT) & readl(IC_INTR_MASK);
	 *
	 * The raw version might be useful for debugging purposes.
	 */
	if (!(dev->flags & ACCESS_POLLING)) {
		regmap_read(dev->map, DWC_IC_INTR_STAT(dev), &stat);
	} else {
		regmap_read(dev->map, DWC_IC_INTR_RAW_STAT(dev), &stat);
		stat &= dev->sw_mask;
	}

	if (stat & DWC_IC_INTR_RX_UNDER_MASK)
		clear_bits |= DWC_IC_INTR_CLR_RX_UNDER_MASK;
	if (stat & DWC_IC_INTR_RX_OVER_MASK)
		clear_bits |= DWC_IC_INTR_CLR_RX_OVER_MASK;
	if (stat & DWC_IC_INTR_TX_OVER_MASK)
		clear_bits |= DWC_IC_INTR_CLR_TX_OVER_MASK;
	if (stat & DWC_IC_INTR_RD_REQ_MASK)
		clear_bits |= DWC_IC_INTR_CLR_RD_REQ_MASK;
	if (stat & DWC_IC_INTR_TX_TRMNT_MASK) {
		/*
		 * The IC_TX_TRMNT_SOURCE register is also cleared whenever
		 * the IC_INTR_TX_TRMNT is cleared. Preserve it beforehand.
		 */
		regmap_read(dev->map, DWC_IC_TX_TRMNT_SOURCE(dev),
			    &dev->trmnt_source);
		clear_bits |= DWC_IC_INTR_CLR_TX_TRMNT_MASK;
	}
	if (stat & DWC_IC_INTR_RX_DONE_MASK)
		clear_bits |= DWC_IC_INTR_CLR_RX_DONE_MASK;
	if (stat & DWC_IC_INTR_ACTIVITY_MASK)
		clear_bits |= DWC_IC_INTR_CLR_ACTIVITY_MASK;
	if ((stat & DWC_IC_INTR_STOP_DET_MASK) &&
	    ((dev->rx_outstanding == 0) || (stat & DWC_IC_INTR_RX_FULL_MASK)))
		clear_bits |= DWC_IC_INTR_CLR_STOP_DET_MASK;
	if (stat & DWC_IC_INTR_START_DET_MASK)
		clear_bits |= DWC_IC_INTR_CLR_START_DET_MASK;
	if (stat & DWC_IC_INTR_GEN_CALL_MASK)
		clear_bits |= DWC_IC_INTR_CLR_GEN_CALL_MASK;
	if (stat & DWC_IC_INTR_RESTART_DET_MASK)
		clear_bits |= DWC_IC_INTR_CLR_RESTART_DET_MASK;
	if (stat & DWC_IC_INTR_SCL_STUCK_DET_MASK)
		clear_bits |= DWC_IC_INTR_CLR_SCL_STUCK_DET_MASK;
	if (stat & DWC_IC_INTR_WR_REQ_MASK)
		clear_bits |= DWC_IC_INTR_CLR_WR_REQ_MASK;
	if (stat & DWC_IC_INTR_TGT_ADDR_TAG_MASK)
		clear_bits |= DWC_IC_INTR_CLR_TGT_ADDR_TAG_MASK;

	regmap_write(dev->map, DWC_IC_INTR_CLR(dev), clear_bits);

	return stat;
}

static u32 dwc_i2c_read_clear_smbus_intrbits(struct dwc_i2c_dev *dev)
{
	unsigned int stat;

	if (!dev->caps.smbus)
		return 0;

	if (!(dev->flags & ACCESS_POLLING)) {
		regmap_read(dev->map, DWC_IC_SMBUS_INTR_STAT(dev), &stat);
	} else {
		regmap_read(dev->map, DWC_IC_SMBUS_INTR_RAW_STAT(dev), &stat);
		stat &= dev->sw_smbus_mask;
	}

	/* clear the caught interrupt factors */
	regmap_write(dev->map, DWC_IC_SMBUS_INTR_CLR(dev), stat);

	return stat;
}

static void dwc_i2c_process_transfer(struct dwc_i2c_dev *dev, unsigned int stat,
				     unsigned int smbus_stat)
{
	if (stat & DWC_IC_INTR_SCL_STUCK_DET_MASK) {
		dev_err(dev->dev, "detected scl stuck at low. Resetting...");
		dev->status |= STATUS_REQ_SOFT_RESET;

		goto req_terminate_or_reset;
	}

	if (smbus_stat & (DWC_IC_SMBUS_INTR_CEXT_TIMEOUT_MASK |
			  DWC_IC_SMBUS_INTR_TEXT_TIMEOUT_MASK)) {
		bool cext_violated = smbus_stat &
				     DWC_IC_SMBUS_INTR_CEXT_TIMEOUT_MASK;
		bool text_violated = smbus_stat &
				     DWC_IC_SMBUS_INTR_TEXT_TIMEOUT_MASK;
		bool violated_both = cext_violated && text_violated;

		dev_err(dev->dev, "detected %s. Terminating transfer...",
			violated_both ? "tLOW:TEXT and tLOW:CEXT violations" :
			cext_violated ? "tLOW:CEXT violation" :
					"tLOW:TEXT violation");
		dev->status |= STATUS_REQ_HANDLING_SMBCLK_EXT_VIOLATION;

		goto req_terminate_or_reset;
	}

	if (stat & DWC_IC_INTR_TX_TRMNT_MASK) {
		dev->cmd_err |= DWC_IC_ERR_TX_TRMNT;
		dev->status &= ~STATUS_MASK;
		dev->rx_outstanding = 0;

		__dwc_i2c_write_intr_mask(dev, 0);
		__dwc_i2c_write_smbus_intr_mask(dev, 0);
		goto tx_terminated;
	}

	if (stat & DWC_IC_INTR_RX_FULL_MASK)
		dwc_i2c_read(dev);

	if (stat & DWC_IC_INTR_TX_EMPTY_MASK)
		dwc_i2c_xfer_msg(dev);

req_terminate_or_reset:
	if (dev->status & (STATUS_REQ_TERMINATE | STATUS_REQ_SOFT_RESET |
			   STATUS_REQ_HANDLING_SMBCLK_EXT_VIOLATION)) {
		dev->rx_outstanding = 0;

		__dwc_i2c_write_intr_mask(dev, 0);
		__dwc_i2c_write_smbus_intr_mask(dev, 0);
	}

tx_terminated:
	if (((stat & (DWC_IC_INTR_TX_TRMNT_MASK | DWC_IC_INTR_STOP_DET_MASK)) ||
	     dev->msg_err ||
	     (dev->status & (STATUS_REQ_TERMINATE | STATUS_REQ_SOFT_RESET |
			     STATUS_REQ_HANDLING_SMBCLK_EXT_VIOLATION))) &&
	    (dev->rx_outstanding == 0)) {
		complete(&dev->cmd_complete);
	} else if (unlikely(dev->flags & ACCESS_INTR_MASK)) {
		/* Workaround to trigger pending interrupt */
		__dwc_i2c_read_intr_mask(dev, &stat);
		__dwc_i2c_read_smbus_intr_mask(dev, &smbus_stat);
		__dwc_i2c_write_intr_mask(dev, 0);
		__dwc_i2c_write_smbus_intr_mask(dev, 0);
		__dwc_i2c_write_intr_mask(dev, stat);
		__dwc_i2c_write_smbus_intr_mask(dev, smbus_stat);
	}
}

/*
 * Interrupt service routine. This gets called whenever an I2C controller
 * interrupt occurs.
 */
static irqreturn_t dwc_i2c_isr(int this_irq, void *dev_id)
{
	struct dwc_i2c_dev *dev = dev_id;
	unsigned int stat, smbus_stat = 0, enabled;

	spin_lock(&dev->lock);

	regmap_read(dev->map, DWC_IC_ENABLE(dev), &enabled);
	regmap_read(dev->map, DWC_IC_INTR_RAW_STAT(dev), &stat);
	if (dev->caps.smbus)
		regmap_read(dev->map, DWC_IC_SMBUS_INTR_RAW_STAT(dev),
			    &smbus_stat);
	if (!enabled || (!(stat & ~DWC_IC_INTR_ACTIVITY_MASK) && !smbus_stat))
		goto none;
	if (pm_runtime_suspended(dev->dev) || stat == GENMASK(31, 0))
		goto none;
	dev_dbg(dev->dev, "enabled=%#x stat=%#x smbus_stat=%#x\n", enabled,
		stat, smbus_stat);

	stat = dwc_i2c_read_clear_intrbits(dev);
	smbus_stat = dwc_i2c_read_clear_smbus_intrbits(dev);
	if (!stat && !smbus_stat)
		goto none;

	if (!(dev->status & STATUS_ACTIVE)) {
		/*
		 * Unexpected interrupt in driver point of view. State
		 * variables are either unset or stale so acknowledge and
		 * disable interrupts for suppressing further interrupts if
		 * interrupt really came from this HW (E.g. firmware has left
		 * the HW active).
		 */
		__dwc_i2c_write_intr_mask(dev, 0);
		__dwc_i2c_write_smbus_intr_mask(dev, 0);
		goto handled;
	}

	dwc_i2c_process_transfer(dev, stat, smbus_stat);

handled:
	spin_unlock(&dev->lock);

	return IRQ_HANDLED;

none:
	spin_unlock(&dev->lock);

	return IRQ_NONE;
}

static int dwc_i2c_wait_transfer(struct dwc_i2c_dev *dev)
{
	unsigned long timeout = dev->adapter.timeout, flags;
	unsigned int stat, smbus_stat;
	int ret;

	if (!(dev->flags & ACCESS_POLLING)) {
		ret = wait_for_completion_timeout(&dev->cmd_complete, timeout);
	} else {
		timeout += jiffies;
		do {
			ret = try_wait_for_completion(&dev->cmd_complete);
			if (ret)
				break;

			stat = dwc_i2c_read_clear_intrbits(dev);
			smbus_stat = dwc_i2c_read_clear_smbus_intrbits(dev);
			if (stat || smbus_stat)
				dwc_i2c_process_transfer(dev, stat, smbus_stat);
			else
				usleep_range(3, 25); /* Try save some power */
		} while (time_before(jiffies, timeout));
	}

	spin_lock_irqsave(&dev->lock, flags);

	/*
	 * The interrupt mask is needed while transferring i2c messages as
	 * follows:
	 * 1) use the interrupt handler (dwc_i2c_isr()),
	 * 2) use dwc_i2c_read_clear_intrbits() (especially in polling mode).
	 * At this point, the current transfer has finished, so the interrupt
	 * should be disabled.
	 */
	__dwc_i2c_write_intr_mask(dev, 0);
	__dwc_i2c_write_smbus_intr_mask(dev, 0);

	spin_unlock_irqrestore(&dev->lock, flags);

	return ret ? 0 : -ETIMEDOUT;
}

static bool __dwc_i2c_terminate_completed(struct dwc_i2c_dev *dev,
					  bool *t_timeout_max_violated)
{
	unsigned int enable, stat;

	regmap_read(dev->map, DWC_IC_INTR_RAW_STAT(dev), &stat);
	*t_timeout_max_violated = stat & DWC_IC_INTR_SCL_STUCK_AT_LOW_MAX_MASK;

	regmap_read(dev->map, DWC_IC_ENABLE(dev), &enable);
	return !(enable & DWC_IC_ENABLE_TERMINATE);
}

static int dwc_i2c_handle_smbclk_extend_violation(struct dwc_i2c_dev *dev)
{
	unsigned int enable;
	int ret;
	bool terminate_completed, t_timeout_max_violated;

	if (WARN_ON(unlikely(!dev->caps.smbus)))
		return -EINVAL;

	/* Make sure the interrupts are disabled. */
	if (WARN_ON(!__dwc_i2c_interrupts_masked(dev)))
		return -EINVAL;

	regmap_read(dev->map, DWC_IC_ENABLE(dev), &enable);
	regmap_write(dev->map, DWC_IC_ENABLE(dev),
		     enable | DWC_IC_ENABLE_TERMINATE);
	ret = read_poll_timeout(__dwc_i2c_terminate_completed,
				terminate_completed,
				terminate_completed || t_timeout_max_violated,
				USEC_PER_MSEC,
				SMBCLK_LOW_TIMEOUT_MAX_MS * USEC_PER_MSEC,
				false, dev, &t_timeout_max_violated);

	if (t_timeout_max_violated)
		return -ETIMEDOUT;
	else
		return ret;
}

/*
 * Prepare controller for a transaction and call dwc_i2c_xfer_msg.
 */
static int __dwc_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg msgs[],
			  int num, bool are_msgs_smbus_quick_cmd)
{
	struct dwc_i2c_dev *dev = i2c_get_adapdata(adap);
	int ret;
	bool has_read_msg = false;
	bool completes_xfer;

	dev_dbg(dev->dev, "%s: msgs: %d\n", __func__, num);

	/*
	 * The read flag for the SMBus Quick Command means the data to be sent,
	 * not the transfer direction.
	 */
	if (!are_msgs_smbus_quick_cmd) {
		for (int i = 0; i < num; i++) {
			if (msgs[i].flags & I2C_M_RD) {
				has_read_msg = true;
				break;
			}
		}
	}

	pm_runtime_get_sync(dev->dev);

	reinit_completion(&dev->cmd_complete);
	dev->msgs = msgs;
	dev->msgs_num = num;
	dev->cmd_err = 0;
	dev->msg_write_idx = 0;
	dev->buf_write_idx = 0;
	/*
	 * When there is no read message in msgs[], dev->msg_read_idx is
	 * initialized as num to represent the read operation has done.
	 */
	dev->msg_read_idx = has_read_msg ? 0 : num;
	dev->buf_read_idx = 0;
	dev->msg_err = 0;
	dev->status = 0;
	dev->trmnt_source = 0;
	dev->rx_outstanding = 0;
	dev->are_msgs_smbus_quick_cmd = are_msgs_smbus_quick_cmd;

	ret = dwc_i2c_wait_bus_not_busy(dev);
	if (ret < 0)
		goto done;

	/* Start the transfers */
	ret = dwc_i2c_xfer_init(dev);
	if (ret)
		goto done;

	/* Wait for tx to complete */
	ret = dwc_i2c_wait_transfer(dev);
	if (ret) {
		dev_err(dev->dev, "controller timed out\n");
		/* ensure to terminate the transfer */
		dwc_i2c_terminate_by_polling(dev);
		/* dwc_i2c_init_controller() implicitly disables the adapter */
		dwc_i2c_init_controller(dev);
		goto done;
	}

	if (dev->status & STATUS_REQ_SOFT_RESET) {
		dwc_i2c_do_reset(dev);
		dwc_i2c_init_controller(dev);
		ret = -EIO;
		goto done;
	}

	if (dev->status & STATUS_REQ_HANDLING_SMBCLK_EXT_VIOLATION) {
		ret = dwc_i2c_handle_smbclk_extend_violation(dev);
		if (ret) {
			dev_err(dev->dev,
				"timeout while trying to terminate tLOW:TEXT- or tLOW:CEXT-violated transfer\n");
			dev_err(dev->dev,
				"the target device can have been hanging up and pulling down the scl line\n");
			dwc_i2c_do_reset(dev);
			dwc_i2c_init_controller(dev);
			ret = -EREMOTEIO;
		} else {
			ret = -EIO;
		}
		goto done;
	}

	if (dev->status & STATUS_REQ_TERMINATE) {
		ret = dwc_i2c_terminate_by_polling(dev);
		if (ret) {
			ret = -EIO;
			goto done;
		}
	}

	completes_xfer = dev->msg_write_idx == num && dev->msg_read_idx == num;
	if (likely(!dev->msg_err && !dev->cmd_err && completes_xfer)) {
		ret = num;
	} else if (dev->msg_err) {
		ret = dev->msg_err;
	} else if (dev->cmd_err == DWC_IC_ERR_TX_TRMNT) {
		ret = dwc_i2c_handle_tx_terminate(dev);
		if (dev->trmnt_source & DWC_IC_TX_TRMNT_SDA_STUCK_AT_LOW_MASK)
			i2c_recover_bus(&dev->adapter);
	} else if (!completes_xfer) {
		dev_err(dev->dev,
			"transfer terminated early - interrupt latency too high?\n");
		ret = -EIO;
	} else {
		dev_err(dev->dev, "Unknown transfer error\n");
		ret = -EIO;
	}

	/*
	 * We must disable the adapter before returning and signaling the end
	 * of the current transfer. Otherwise the hardware might continue
	 * generating interrupts which in turn causes a race condition with
	 * the following transfer. Needs some more investigation if the
	 * additional interrupts are a hardware bug or this driver doesn't
	 * handle them correctly yet.
	 */
	__dwc_i2c_disable_nowait(dev);

done:
	pm_runtime_mark_last_busy(dev->dev);
	pm_runtime_put_autosuspend(dev->dev);

	return ret;
}

static int dwc_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg msgs[],
			int num)
{
	return __dwc_i2c_xfer(adap, msgs, num, false);
}

#define DWC_I2C_QUICK_CMD_NUM_MSGS 1
static int dwc_i2c_smbus_quick_cmd(struct i2c_adapter *adap, u16 addr,
				   unsigned short flags, char read_write)
{
	int ret;
	struct i2c_msg msg[DWC_I2C_QUICK_CMD_NUM_MSGS] = {
		{
			.addr = addr,
			.flags = flags |
				 (read_write == I2C_SMBUS_READ ? I2C_M_RD : 0),
			.len = 1,

			/* Make sure not to use buf while len is 1. */
			.buf = NULL,
		},
	};

	ret = __dwc_i2c_xfer(adap, msg, DWC_I2C_QUICK_CMD_NUM_MSGS, true);

	if (ret < 0)
		return ret;
	else if (ret != DWC_I2C_QUICK_CMD_NUM_MSGS)
		return -EIO;
	else
		return 0;
}

static int dwc_i2c_smbus_xfer(struct i2c_adapter *adap, u16 addr,
			      unsigned short flags, char read_write, u8 command,
			      int protocol, union i2c_smbus_data *data)
{
	struct dwc_i2c_dev *dev = i2c_get_adapdata(adap);
	int ret;

	switch (protocol) {
	case I2C_SMBUS_QUICK:
		if (!dev->caps.smbus) {
			dev_err(dev->dev, "Quick Command is not supported\n");
			return -EINVAL;
		}
		ret = dwc_i2c_smbus_quick_cmd(adap, addr, flags, read_write);
		break;
	default:
		dev_dbg(dev->dev,
			"delegate the operation to the emulation with master_xfer\n");
		ret = -EOPNOTSUPP;
	}

	return ret;
}

static const struct i2c_algorithm dwc_i2c_algo = {
	.master_xfer = dwc_i2c_xfer,
	.smbus_xfer = dwc_i2c_smbus_xfer,
	.functionality = dwc_i2c_func,
};

static const struct i2c_adapter_quirks dwc_i2c_quirks = {
	.flags = I2C_AQ_NO_ZERO_LEN,
};

void dwc_i2c_configure_controller(struct dwc_i2c_dev *dev)
{
	u32 f = dev->timings.bus_freq_hz;
	u32 speed;

	dev->functionality = DWC_IC_DEFAULT_FUNCTIONALITY;
	if (dev->caps.supports_10bit_addr_ctrlr)
		dev->functionality |= I2C_FUNC_10BIT_ADDR;
	if (dev->caps.smbus)
		dev->functionality |= I2C_FUNC_SMBUS_QUICK;
	if (!dev->caps.ctrlr_holds_bus_with_tx_fifo_empty)
		dev->functionality &= ~(I2C_FUNC_SMBUS_READ_BLOCK_DATA |
					I2C_FUNC_SMBUS_BLOCK_PROC_CALL);

	dev->mode = DWC_IC_CTRL_OP_MODE_CONTROLLER;

	speed = (f == I2C_MAX_STANDARD_MODE_FREQ)   ? DWC_IC_CTRL_SPEED_STD :
		(f == I2C_MAX_HIGH_SPEED_MODE_FREQ) ? DWC_IC_CTRL_SPEED_HIGH :
						      DWC_IC_CTRL_SPEED_FAST;

	dev->controller_cfg = FIELD_PREP(DWC_IC_CTRL_SPEED_MASK, speed) |
			      FIELD_PREP(DWC_IC_CTRL_OP_MODE_MASK, dev->mode);
	if (dev->caps.supports_bus_clear)
		dev->controller_cfg |= DWC_IC_CTRL_BUS_CLEAR_FEATURE_CTRL;
}

#define SCL_NINE_CLOCKS_US(bus_freq) DIV_ROUND_UP(9 * MICRO, bus_freq)
static int dwc_i2c_recover_i2c_sda_line(struct dwc_i2c_dev *dev)
{
	u32 interval = SCL_NINE_CLOCKS_US(dev->timings.bus_freq_hz);
	unsigned int val, status;

	if (WARN_ON(unlikely(!dev->caps.supports_bus_clear)))
		return -EOPNOTSUPP;

	regmap_set_bits(dev->map, DWC_IC_ENABLE(dev),
			DWC_IC_ENABLE_SDA_STUCK_RECOVERY_ENABLE);

	regmap_read_poll_timeout(dev->map, DWC_IC_ENABLE(dev), val,
				 !(val &
				   DWC_IC_ENABLE_SDA_STUCK_RECOVERY_ENABLE),
				 interval, interval * 100);

	regmap_read(dev->map, DWC_IC_STATUS(dev), &status);
	if (status & DWC_IC_STATUS_SDA_STUCK_NOT_RECOVERED) {
		dev_err(dev->dev, "failed to recover the sda line\n");
		return -EREMOTEIO;
	}

	return 0;
}

static int dwc_i2c_reset_smbus(struct dwc_i2c_dev *dev)
{
	unsigned int val;
	int ret;
	unsigned long t_timeout_max = SMBCLK_LOW_TIMEOUT_MAX_MS * USEC_PER_MSEC;

	if (WARN_ON(unlikely(!dev->caps.smbus ||
			     !dev->caps.supports_bus_clear)))
		return -EOPNOTSUPP;

	regmap_set_bits(dev->map, DWC_IC_SMBUS_CTRL(dev),
			DWC_IC_SMBUS_CTRL_SMBUS_CLK_RESET);

	ret = regmap_read_poll_timeout(dev->map, DWC_IC_SMBUS_CTRL(dev), val,
				       !(val &
					 DWC_IC_SMBUS_CTRL_SMBUS_CLK_RESET),
				       t_timeout_max / 10, t_timeout_max * 10);
	if (ret) {
		dev_err(dev->dev, "timed out to reset the smbus\n");
		return ret;
	}

	return 0;
}

static int dwc_i2c_recover_bus(struct i2c_adapter *adap)
{
	struct dwc_i2c_dev *dev = i2c_get_adapdata(adap);

	if (!dev->caps.supports_bus_clear)
		return -EOPNOTSUPP;

	return dev->is_smbus_mode ? dwc_i2c_reset_smbus(dev) :
				    dwc_i2c_recover_i2c_sda_line(dev);
}

static void dwc_i2c_init_recovery_info(struct dwc_i2c_dev *dev)
{
	struct i2c_bus_recovery_info *rinfo = &dev->rinfo;
	struct i2c_adapter *adap = &dev->adapter;

	if (!dev->caps.supports_bus_clear)
		return;

	rinfo->recover_bus = dwc_i2c_recover_bus;
	adap->bus_recovery_info = rinfo;
}

int dwc_i2c_probe_controller(struct dwc_i2c_dev *dev)
{
	struct i2c_adapter *adap = &dev->adapter;
	unsigned long irq_flags;
	int ret;

	init_completion(&dev->cmd_complete);
	spin_lock_init(&dev->lock);

	dev->init = dwc_i2c_init_controller;

	ret = dwc_i2c_set_timings_controller(dev);
	if (ret)
		return ret;

	ret = dev->init(dev);
	if (ret)
		return ret;

	snprintf(adap->name, sizeof(adap->name),
		 "Synopsys DesignWare Core I2C adapter");
	adap->retries = 3;
	adap->algo = &dwc_i2c_algo;
	adap->quirks = &dwc_i2c_quirks;
	adap->dev.parent = dev->dev;
	i2c_set_adapdata(adap, dev);

	if (dev->flags & ACCESS_NO_IRQ_SUSPEND)
		irq_flags = IRQF_NO_SUSPEND;
	else
		irq_flags = IRQF_SHARED | IRQF_COND_SUSPEND;

	__dwc_i2c_write_intr_mask(dev, 0);
	__dwc_i2c_write_smbus_intr_mask(dev, 0);

	if (!(dev->flags & ACCESS_POLLING)) {
		ret = devm_request_irq(dev->dev, dev->irq, dwc_i2c_isr,
				       irq_flags, dev_name(dev->dev), dev);
		if (ret) {
			dev_err(dev->dev, "failure requesting irq %i: %d\n",
				dev->irq, ret);
			return ret;
		}
	}

	dwc_i2c_init_recovery_info(dev);

	/*
	 * Increment PM usage count during adapter registration in order to
	 * avoid possible spurious runtime suspend when adapter device is
	 * registered to the device core and immediate resume in case bus has
	 * registered I2C slaves that do I2C transfers in their probe.
	 */
	pm_runtime_get_noresume(dev->dev);
	ret = i2c_add_numbered_adapter(adap);
	if (ret)
		dev_err(dev->dev, "failure adding adapter: %d\n", ret);
	pm_runtime_put_noidle(dev->dev);

	return ret;
}

MODULE_DESCRIPTION("Synopsys DesignWare Core I2C bus controller adapter");
MODULE_LICENSE("GPL");
