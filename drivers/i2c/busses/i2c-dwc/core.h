/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Synopsys DesignWare Core I2C adapter driver.
 *
 * Copyright (C) 2024 Socionext Inc.
 * Portions Copyright (c) 2024 Synopsys, Inc. Used with permission. All rights reserved.
 *
 * This is a derivative work of Synopsys DesignWare I2C adapter driver.
 * - drivers/i2c/busses/i2c-designware-core.h
 *
 * Copyright (C) 2006 Texas Instruments.
 * Copyright (C) 2007 MontaVista Software Inc.
 * Copyright (C) 2009 Provigent Ltd.
 */

#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/compiler_types.h>
#include <linux/completion.h>
#include <linux/dev_printk.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#define DWC_IC_DEFAULT_FUNCTIONALITY \
	((I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL_ALL) & ~I2C_FUNC_SMBUS_QUICK)

/* helpers to generate register offset */
#define __DWC_I2C_NON_OP_REG(i2c_dev, member, addr) ({			\
	typeof(i2c_dev) _i2c_dev = (i2c_dev);				\
	u32 _addr = (addr);						\
	static_assert(__same_type(i2c_dev, struct dwc_i2c_dev *));	\
	_i2c_dev->member + _addr;					\
})
#define _DWC_I2C_I2C_REG(i2c_dev, addr)		\
	__DWC_I2C_NON_OP_REG(i2c_dev, i2c_reg_offset, addr)
#define _DWC_I2C_SMBUS_REG(i2c_dev, addr)	\
	__DWC_I2C_NON_OP_REG(i2c_dev, smbus_reg_offset, addr)

/*
 * Operational Block Registers offset
 */
#define DWC_IC_ENABLE(dev)				0x4
#define DWC_IC_RESET_CTRL(dev)				0x8
#define DWC_IC_CAPABILITIES(dev)			0xc
#define DWC_IC_I2C_CAPABILITIES(dev)			0x10
#define DWC_IC_I2C_BLOCK_OFFSET(dev)			0x14
#define DWC_IC_SMBUS_CAPABILITIES(dev)			0x18
#define DWC_IC_SMBUS_BLOCK_OFFSET(dev)			0x1c

#define DWC_IC_ENABLE_ENABLE				BIT(0)
#define DWC_IC_ENABLE_TERMINATE				BIT(1)
#define DWC_IC_ENABLE_SDA_STUCK_RECOVERY_ENABLE		BIT(3)

#define DWC_IC_RESET_CTRL_SOFT_RESET			BIT(0)

#define DWC_IC_CAPABILITIES_SMBUS			BIT(10)

#define DWC_IC_I2C_CAPABILITIES_BUS_CLEAR_FEATURE	BIT(4)
#define DWC_IC_I2C_CAPABILITIES_CAP_LOADING		BIT(3)
#define DWC_IC_I2C_CAPABILITIES_10BITADDR_CTRLR		BIT(2)
#define DWC_IC_I2C_CAPABILITIES_10BITADDR_TGT		BIT(1)
#define DWC_IC_I2C_CAPABILITIES_ULTRA_FAST_MODE		BIT(0)

/*
 * I2C Block Registers offset and bit fields
 */
#define DWC_IC_CTRL(dev)			 _DWC_I2C_I2C_REG(dev, 0x4)
#define DWC_IC_TAR(dev)				 _DWC_I2C_I2C_REG(dev, 0x8)
#define DWC_IC_DAR(dev)				 _DWC_I2C_I2C_REG(dev, 0xc)
#define DWC_IC_SCL_HCNT(dev)			 _DWC_I2C_I2C_REG(dev, 0x24)
#define DWC_IC_SCL_LCNT(dev)			 _DWC_I2C_I2C_REG(dev, 0x28)
#define DWC_IC_HS_SCL_HCNT(dev)			 _DWC_I2C_I2C_REG(dev, 0x2c)
#define DWC_IC_HS_SCL_LCNT(dev)			 _DWC_I2C_I2C_REG(dev, 0x30)
#define DWC_IC_SDA_HOLD(dev)			 _DWC_I2C_I2C_REG(dev, 0x34)
#define DWC_IC_SDA_SETUP(dev)			 _DWC_I2C_I2C_REG(dev, 0x38)
#define DWC_IC_SPKLEN(dev)			 _DWC_I2C_I2C_REG(dev, 0x3c)
#define DWC_IC_HS_SPKLEN(dev)			 _DWC_I2C_I2C_REG(dev, 0x40)
#define DWC_IC_SCL_STUCK_AT_LOW_TIMEOUT(dev)	 _DWC_I2C_I2C_REG(dev, 0x44)
#define DWC_IC_SCL_STUCK_AT_LOW_TIMEOUT_MAX(dev) _DWC_I2C_I2C_REG(dev, 0x48)
#define DWC_IC_SDA_STUCK_AT_LOW_TIMEOUT(dev)	 _DWC_I2C_I2C_REG(dev, 0x4c)
#define DWC_IC_DATA_CMD(dev)			 _DWC_I2C_I2C_REG(dev, 0x58)
#define DWC_IC_RX_TL(dev)			 _DWC_I2C_I2C_REG(dev, 0x5c)
#define DWC_IC_TX_TL(dev)			 _DWC_I2C_I2C_REG(dev, 0x60)
#define DWC_IC_INTR_STAT(dev)			 _DWC_I2C_I2C_REG(dev, 0x74)
#define DWC_IC_INTR_MASK(dev)			 _DWC_I2C_I2C_REG(dev, 0x78)
#define DWC_IC_INTR_RAW_STAT(dev)		 _DWC_I2C_I2C_REG(dev, 0x7c)
#define DWC_IC_INTR_CLR(dev)			 _DWC_I2C_I2C_REG(dev, 0x80)
#define DWC_IC_ENABLE_STATUS(dev)		 _DWC_I2C_I2C_REG(dev, 0x84)
#define DWC_IC_TX_TRMNT_SOURCE(dev)		 _DWC_I2C_I2C_REG(dev, 0x88)
#define DWC_IC_STATUS(dev)			 _DWC_I2C_I2C_REG(dev, 0x8c)
#define DWC_IC_TXFLR(dev)			 _DWC_I2C_I2C_REG(dev, 0x90)
#define DWC_IC_RXFLR(dev)			 _DWC_I2C_I2C_REG(dev, 0x94)
#define DWC_IC_COMP_VERSION(dev)		 _DWC_I2C_I2C_REG(dev, 0xa4)
#define DWC_IC_COMP_TYPE(dev)			 _DWC_I2C_I2C_REG(dev, 0xa8)

#define DWC_IC_CTRL_OP_MODE_MASK		BIT(0)
enum dwc_i2c_op_mode {
	DWC_IC_CTRL_OP_MODE_CONTROLLER =	1,
	DWC_IC_CTRL_OP_MODE_TARGET =		0,
};
#define DWC_IC_CTRL_SPEED_MASK			GENMASK(5, 4)
enum dwc_i2c_speed {
	DWC_IC_CTRL_SPEED_STD =			1,
	DWC_IC_CTRL_SPEED_FAST =		2,
	DWC_IC_CTRL_SPEED_HIGH =		3,
};
#define DWC_IC_CTRL_10BITADDR_TGT		BIT(8)
#define DWC_IC_CTRL_10BITADDR_CTRLR		BIT(9)
#define DWC_IC_CTRL_STOP_DET_IFADDRESSED	BIT(10)
#define DWC_IC_CTRL_TX_EMPTY_CTRL		BIT(11)
#define DWC_IC_CTRL_RX_FIFO_FULL_HLD_CTRL	BIT(12)
#define DWC_IC_CTRL_BUS_CLEAR_FEATURE_CTRL	BIT(14)

#define DWC_IC_DATA_CMD_DAT_MASK		GENMASK(7, 0)
#define DWC_IC_DATA_CMD_READ_CMD		BIT(8)
#define DWC_IC_DATA_CMD_STOP			BIT(9)
#define DWC_IC_DATA_CMD_RESTART			BIT(10)

#define DWC_IC_INTR_RX_UNDER_MASK		BIT(0)
#define DWC_IC_INTR_RX_OVER_MASK		BIT(1)
#define DWC_IC_INTR_RX_FULL_MASK		BIT(2)
#define DWC_IC_INTR_TX_OVER_MASK		BIT(3)
#define DWC_IC_INTR_TX_EMPTY_MASK		BIT(4)
#define DWC_IC_INTR_RD_REQ_MASK			BIT(5)
#define DWC_IC_INTR_TX_TRMNT_MASK		BIT(6)
#define DWC_IC_INTR_RX_DONE_MASK		BIT(7)
#define DWC_IC_INTR_ACTIVITY_MASK		BIT(8)
#define DWC_IC_INTR_STOP_DET_MASK		BIT(9)
#define DWC_IC_INTR_START_DET_MASK		BIT(10)
#define DWC_IC_INTR_GEN_CALL_MASK		BIT(11)
#define DWC_IC_INTR_RESTART_DET_MASK		BIT(12)
#define DWC_IC_INTR_CTRLR_ON_HOLD_MASK		BIT(13)
#define DWC_IC_INTR_SCL_STUCK_AT_LOW_MASK	BIT(14)
#define DWC_IC_INTR_WR_REQ_MASK			BIT(15)
#define DWC_IC_INTR_TGT_ADDR_X_TAG_MASK(x)	BIT(16 + (x))
#define DWC_IC_INTR_SCL_STUCK_AT_LOW_MAX_MASK	BIT(24)

#define DWC_IC_INTR_DEFAULT_MASK		(DWC_IC_INTR_RX_FULL_MASK | \
						 DWC_IC_INTR_TX_TRMNT_MASK | \
						 DWC_IC_INTR_STOP_DET_MASK)
#define DWC_IC_INTR_CONTROLLER_MASK		(DWC_IC_INTR_DEFAULT_MASK | \
						 DWC_IC_INTR_TX_EMPTY_MASK)
#define DWC_IC_INTR_TARGET_MASK			(DWC_IC_INTR_DEFAULT_MASK | \
						 DWC_IC_INTR_RX_DONE_MASK | \
						 DWC_IC_INTR_RX_UNDER_MASK | \
						 DWC_IC_INTR_RD_REQ_MASK)

#define DWC_IC_INTR_SCL_STUCK_DET_MASK	(DWC_IC_INTR_SCL_STUCK_AT_LOW_MASK | \
					 DWC_IC_INTR_SCL_STUCK_AT_LOW_MAX_MASK)
#define DWC_IC_INTR_TGT_ADDR_TAG_MASK	(DWC_IC_INTR_TGT_ADDR_X_TAG_MASK(0) | \
					 DWC_IC_INTR_TGT_ADDR_X_TAG_MASK(1) | \
					 DWC_IC_INTR_TGT_ADDR_X_TAG_MASK(2) | \
					 DWC_IC_INTR_TGT_ADDR_X_TAG_MASK(3))

#define DWC_IC_INTR_CLR_INTR_MASK		BIT(0)
#define DWC_IC_INTR_CLR_RX_UNDER_MASK		BIT(1)
#define DWC_IC_INTR_CLR_RX_OVER_MASK		BIT(2)
#define DWC_IC_INTR_CLR_TX_OVER_MASK		BIT(3)
#define DWC_IC_INTR_CLR_RD_REQ_MASK		BIT(4)
#define DWC_IC_INTR_CLR_TX_TRMNT_MASK		BIT(5)
#define DWC_IC_INTR_CLR_RX_DONE_MASK		BIT(6)
#define DWC_IC_INTR_CLR_ACTIVITY_MASK		BIT(7)
#define DWC_IC_INTR_CLR_STOP_DET_MASK		BIT(8)
#define DWC_IC_INTR_CLR_START_DET_MASK		BIT(9)
#define DWC_IC_INTR_CLR_GEN_CALL_MASK		BIT(10)
#define DWC_IC_INTR_CLR_RESTART_DET_MASK	BIT(11)
#define DWC_IC_INTR_CLR_SCL_STUCK_DET_MASK	BIT(12)
#define DWC_IC_INTR_CLR_WR_REQ_MASK		BIT(13)
#define DWC_IC_INTR_CLR_TGT_ADDR_TAG_MASK	BIT(14)

#define DWC_IC_ENABLE_STATUS_IC_EN_MASK		BIT(0)

#define DWC_IC_STATUS_ACTIVITY_MASK		BIT(0)
#define DWC_IC_STATUS_TFE_MASK			BIT(2)
#define DWC_IC_STATUS_CTRLR_ACTIVITY_MASK	BIT(5)
#define DWC_IC_STATUS_TGT_ACTIVITY_MASK		BIT(6)
#define DWC_IC_STATUS_SDA_STUCK_NOT_RECOVERED	BIT(11)

#define DWC_IC_SDA_HOLD_TX_MASK			GENMASK(15, 0)
#define DWC_IC_SDA_HOLD_RX_MASK			GENMASK(23, 16)

#define DWC_IC_TAR_IC_TAR_MASK			GENMASK(9, 0)
#define DWC_IC_TAR_SPECIAL			BIT(11)
#define DWC_IC_TAR_10BITADDR_CTRLR		BIT(12)
#define DWC_IC_TAR_SMBUS_QUICK_CMD		BIT(16)

#define DWC_IC_SDA_SETUP_MASK			GENMASK(7, 0)

#define DWC_IC_COMP_TYPE_VALUE			0x44570140 /* "DW" + 0x0140 */

/*
 * Hardware terminate codes from the DW_IC_TX_TRMNT_SOURCE register
 *
 * Only expected terminate codes are listed here,
 * refer to the datasheet for the full list.
 */

#define TRMNT_7B_ADDR_NOACK_IDX			0
#define TRMNT_10ADDR1_NOACK_IDX			1
#define TRMNT_10ADDR2_NOACK_IDX			2
#define TRMNT_TXDATA_NOACK_IDX			3
#define TRMNT_GCALL_NOACK_IDX			4
#define TRMNT_GCALL_READ_IDX			5
#define TRMNT_HS_ACKDET				6
#define TRMNT_SBYTE_ACKDET_IDX			7
#define TRMNT_CTRLR_DIS_IDX			11
#define TRMNT_ARB_LOST_IDX			12
#define TRMNT_TGTFLUSH_TXFIFO_IDX		13
#define TRMNT_TGT_ARBLOST_IDX			14
#define TRMNT_TGTRD_INTX_IDX			15
#define TRMNT_USER_TRMNT_IDX			16
#define TRMNT_SDA_STUCK_AT_LOW_IDX		17

#define DWC_IC_TX_TRMNT_7B_ADDR_NOACK_MASK	BIT(TRMNT_7B_ADDR_NOACK_IDX)
#define DWC_IC_TX_TRMNT_10ADDR1_NOACK_MASK	BIT(TRMNT_10ADDR1_NOACK_IDX)
#define DWC_IC_TX_TRMNT_10ADDR2_NOACK_MASK	BIT(TRMNT_10ADDR2_NOACK_IDX)
#define DWC_IC_TX_TRMNT_TXDATA_NOACK_MASK	BIT(TRMNT_TXDATA_NOACK_IDX)
#define DWC_IC_TX_TRMNT_GCALL_NOACK_MASK	BIT(TRMNT_GCALL_NOACK_IDX)
#define DWC_IC_TX_TRMNT_GCALL_READ_MASK		BIT(TRMNT_GCALL_READ_IDX)
#define DWC_IC_TX_TRMNT_HS_ACKDET_MASK		BIT(TRMNT_HS_ACKDET)
#define DWC_IC_TX_TRMNT_SBYTE_ACKDET_MASK	BIT(TRMNT_SBYTE_ACKDET_IDX)
#define DWC_IC_TX_TRMNT_CTRLR_DIS_MASK		BIT(TRMNT_CTRLR_DIS_IDX)
#define DWC_IC_TX_TRMNT_ARB_LOST_MASK		BIT(TRMNT_ARB_LOST_IDX)
#define DWC_IC_TX_TRMNT_TGTFLUSH_TXFIFO_MASK	BIT(TRMNT_TGTFLUSH_TXFIFO_IDX)
#define DWC_IC_TX_TRMNT_TGT_ARBLOST_MASK	BIT(TRMNT_TGT_ARBLOST_IDX)
#define DWC_IC_TX_TRMNT_TGTRD_INTX_MASK		BIT(TRMNT_TGTRD_INTX_IDX)
#define DWC_IC_TX_TRMNT_USER_TRMNT_MASK		BIT(TRMNT_USER_TRMNT_IDX)
#define DWC_IC_TX_TRMNT_SDA_STUCK_AT_LOW_MASK	BIT(TRMNT_SDA_STUCK_AT_LOW_IDX)

#define IC_TX_TRMNT_SOURCE_NOACK_MASK (DWC_IC_TX_TRMNT_7B_ADDR_NOACK_MASK | \
				       DWC_IC_TX_TRMNT_10ADDR1_NOACK_MASK | \
				       DWC_IC_TX_TRMNT_10ADDR2_NOACK_MASK | \
				       DWC_IC_TX_TRMNT_TXDATA_NOACK_MASK | \
				       DWC_IC_TX_TRMNT_GCALL_NOACK_MASK)

/*
 * SMBus Block Registers offset and bit fields
 */
#define DWC_IC_SMBUS_CTRL(dev)			_DWC_I2C_SMBUS_REG(dev, 0x4)
#define DWC_IC_SMBUS_CLK_LOW_TEXT(dev)		_DWC_I2C_SMBUS_REG(dev, 0x18)
#define DWC_IC_SMBUS_CLK_LOW_CEXT(dev)		_DWC_I2C_SMBUS_REG(dev, 0x1c)
#define DWC_IC_SMBUS_THIGH_MAX_IDLE_COUNT(dev)	_DWC_I2C_SMBUS_REG(dev, 0x20)
#define DWC_IC_SMBUS_INTR_STAT(dev)		_DWC_I2C_SMBUS_REG(dev, 0x28)
#define DWC_IC_SMBUS_INTR_MASK(dev)		_DWC_I2C_SMBUS_REG(dev, 0x2c)
#define DWC_IC_SMBUS_INTR_RAW_STAT(dev)		_DWC_I2C_SMBUS_REG(dev, 0x30)
#define DWC_IC_SMBUS_INTR_CLR(dev)		_DWC_I2C_SMBUS_REG(dev, 0x34)

#define DWC_IC_SMBUS_CTRL_SMBUS_CLK_RESET	BIT(8)

#define DWC_IC_SMBUS_INTR_TEXT_TIMEOUT_MASK	BIT(0)
#define DWC_IC_SMBUS_INTR_CEXT_TIMEOUT_MASK	BIT(1)

#define DWC_IC_SMBUS_INTR_CLR_ALL		GENMASK(31, 0)

/*
 * Sofware status flags
 */
#define STATUS_IDLE						BIT(0)
#define STATUS_ACTIVE					BIT(1)
#define STATUS_REQ_TERMINATE				BIT(2)
#define STATUS_REQ_SOFT_RESET				BIT(3)
#define STATUS_REQ_HANDLING_SMBCLK_EXT_VIOLATION	BIT(4)
#define STATUS_WRITE_IN_PROGRESS			BIT(5)
#define STATUS_READ_IN_PROGRESS				BIT(6)
#define STATUS_MASK					GENMASK(6, 0)

#define DWC_IC_ERR_TX_TRMNT				0x1

/**
 * struct dwc_i2c_capabilities - the information of the DWC_I2C capabilities
 * @supports_bus_clear: supports the bus clear feature
 * @supports_10bit_addr_ctrlr: support 10bit address controller
 * @max_speed_mode: the max bus speed, DWC_IC_CTRL_SPEED_FAST or DWC_IC_CTRL_SPEED_HIGH
 * @ctrlr_holds_bus_with_tx_fifo_empty: hold the bus clock when tx fifo is empty
 * @smbus: support SMBus
 */
struct dwc_i2c_capabilities {
	bool supports_bus_clear;
	bool supports_10bit_addr_ctrlr;
	enum dwc_i2c_speed max_speed_mode;
	bool ctrlr_holds_bus_with_tx_fifo_empty;
	bool smbus;
};

struct clk;
struct device;
struct reset_control;

/**
 * struct dwc_i2c_dev - private i2c-dwc data
 * @dev: driver model device node
 * @map: IO registers map
 * @base: IO registers pointer
 * @reg_size: the size of the registers
 * @i2c_reg_offset: the offset of the i2c register block
 * @smbus_reg_offset: the offset of the SMBus register block
 * @cmd_complete: tx completion indicator
 * @lock: arbitrator between IRQ and completion
 * @clk: input reference clock
 * @pclk: clock required to access the registers
 * @rst: optional reset for the controller
 * @target: represent an I2C target device
 * @get_clk_rate_khz: callback to retrieve IP specific bus speed
 * @cmd_err: run time hardware error code
 * @msgs: points to an array of messages currently being transferred
 * @msgs_num: the number of elements in msgs
 * @are_msgs_smbus_quick_cmd: whether the current messages are the Quick Command
 * @msg_write_idx: the element index of the current tx message in the msgs array
 * @buf_write_idx: the index of current buf in the msg_write_idx-th msgs
 * @msg_read_idx: the element index of the current rx message in the msgs array
 * @buf_read_idx: the index of current buf in the msg_read_idx-th msgs
 * @msg_err: error status of the current transfer
 * @status: i2c status, one of STATUS_*
 * @trmnt_source: copy of the TX_TRMNT_SOURCE register
 * @sw_mask: SW mask of DWC_IC_INTR_MASK used in polling mode
 * @sw_smbus_mask: SW mask of DWC_IC_SMBUS_INTR_MASK used in polling mode
 * @irq: interrupt number for the i2c
 * @flags: platform specific flags like type of IO accessors or model
 * @adapter: i2c subsystem adapter node
 * @functionality: I2C_FUNC_* ORed bits to reflect what controller does support
 * @controller_cfg: configuration for the controller device
 * @target_cfg: configuration for the target device
 * @tx_fifo_depth: depth of the hardware tx fifo
 * @rx_fifo_depth: depth of the hardware rx fifo
 * @rx_outstanding: current controller-rx elements in tx fifo
 * @timings: bus clock frequency, SDA hold and other timings
 * @sda_hold_time: SDA hold value
 * @sda_setup_time: SDA setup value
 * @spklen_cnt: the longest spike to be suppressed in ic_clk cycle (SS, FS, FS+)
 * @hs_spklen_cnt: the longest spike to be suppressed in ic_clk cycle (HS)
 * @ss_hcnt: standard speed HCNT value
 * @ss_lcnt: standard speed LCNT value
 * @fs_hcnt: fast speed HCNT value
 * @fs_lcnt: fast speed LCNT value
 * @fp_hcnt: fast plus HCNT value
 * @fp_lcnt: fast plus LCNT value
 * @hs_hcnt: high speed HCNT value
 * @hs_lcnt: high speed LCNT value
 * @disable: function to disable the I2C hardware
 * @disable_int: function to disable interrupt the I2C hardware
 * @init: function to initialize the I2C hardware
 * @mode: operation mode
 * @caps: the information of the DWC_I2C capabilities
 * @rinfo: I2C recovery information
 * @is_smbus_mode: operate as a SMBus device
 * @ic_clk_freq_khz: DWC_i2c core clock frequency
 *
 * HCNT and LCNT parameters can be used if the platform knows more accurate
 * values than the one computed based only on the input clock frequency.
 * Leave them to be %0 if not used.
 */
struct dwc_i2c_dev {
	struct device		*dev;
	struct regmap		*map;
	void __iomem		*base;
	resource_size_t		reg_size;
	u32			i2c_reg_offset;
	u32			smbus_reg_offset;
	struct completion	cmd_complete;
	spinlock_t		lock;
	struct clk		*clk;
	struct clk		*pclk;
	struct reset_control	*rst;
	struct i2c_client	*target;
	u32			(*get_clk_rate_khz)(struct dwc_i2c_dev *dev);
	int			cmd_err;
	struct i2c_msg		*msgs;
	int			msgs_num;
	bool			are_msgs_smbus_quick_cmd;
	int			msg_write_idx;
	int			buf_write_idx;
	int			msg_read_idx;
	int			buf_read_idx;
	int			msg_err;
	unsigned int		status;
	unsigned int		trmnt_source;
	unsigned int		sw_mask;
	unsigned int		sw_smbus_mask;
	int			irq;
	u32			flags;
	struct i2c_adapter	adapter;
	u32			functionality;
	u32			controller_cfg;
	u32			target_cfg;
	unsigned int		tx_fifo_depth;
	unsigned int		rx_fifo_depth;
	int			rx_outstanding;
	struct i2c_timings	timings;
	u32			sda_hold_time;
	u32			sda_setup_time;
	u8			spklen_cnt;
	u8			hs_spklen_cnt;
	u16			ss_hcnt;
	u16			ss_lcnt;
	u16			fs_hcnt;
	u16			fs_lcnt;
	u16			fp_hcnt;
	u16			fp_lcnt;
	u16			hs_hcnt;
	u16			hs_lcnt;
	void			(*disable)(struct dwc_i2c_dev *dev);
	void			(*disable_int)(struct dwc_i2c_dev *dev);
	int			(*init)(struct dwc_i2c_dev *dev);
	enum dwc_i2c_op_mode	mode;
	struct dwc_i2c_capabilities caps;
	struct i2c_bus_recovery_info rinfo;
	bool			is_smbus_mode;
	u32			ic_clk_freq_khz;
};

#define ACCESS_INTR_MASK			BIT(0)
#define ACCESS_NO_IRQ_SUSPEND			BIT(1)
#define ACCESS_POLLING				BIT(2)

#define DWC_MIN_TX_FIFO_DEPTH			2
#define DWC_MAX_TX_FIFO_DEPTH			256
#define DWC_MIN_RX_FIFO_DEPTH			2
#define DWC_MAX_RX_FIFO_DEPTH			256

#define DWC_IC_SPEED_ID_TO_STR(speed)					\
	((speed == DWC_IC_CTRL_SPEED_STD)  ? "STANDARD" :		\
	 (speed == DWC_IC_CTRL_SPEED_FAST) ? "FAST" :			\
	 (speed == DWC_IC_CTRL_SPEED_HIGH) ? "HIGH" : "INVALID")

#define SMBCLK_LOW_TIMEOUT_MS		25
#define SMBCLK_LOW_TIMEOUT_MAX_MS	35
#define SMBUS_LOW_TEXT_MS		25
#define SMBUS_LOW_CEXT_MS		10
#define SMBUS_THIGH_MAX_US		50

u32 dwc_i2c_scl_hcnt(u32 ic_clk, u32 tSYMBOL, u32 tfda, u32 padding, int offset);
u32 dwc_i2c_scl_lcnt(u32 ic_clk, u32 tLOW, u32 tfcl, u32 tfda, u32 padding, int offset);
int dwc_i2c_set_sda_hold(struct dwc_i2c_dev *dev);
int dwc_i2c_set_sda_setup(struct dwc_i2c_dev *dev);
u32 dwc_i2c_clk_rate(struct dwc_i2c_dev *dev);
int dwc_i2c_prepare_clk(struct dwc_i2c_dev *dev, bool prepare);
int dwc_i2c_wait_bus_not_busy(struct dwc_i2c_dev *dev);
int dwc_i2c_handle_tx_terminate(struct dwc_i2c_dev *dev);
u32 dwc_i2c_func(struct i2c_adapter *adap);
void dwc_i2c_disable(struct dwc_i2c_dev *dev);
void dwc_i2c_disable_int(struct dwc_i2c_dev *dev);

static inline void __dwc_i2c_enable(struct dwc_i2c_dev *dev)
{
	dev->status |= STATUS_ACTIVE;
	regmap_set_bits(dev->map, DWC_IC_ENABLE(dev), DWC_IC_ENABLE_ENABLE);
}

static inline void __dwc_i2c_disable_nowait(struct dwc_i2c_dev *dev)
{
	/*
	 * Not to use the bit field update operations to avoid resetting the
	 * TERMINATE and/or SDA_STUCK_RECOVERY_ENABLE bits just after the HW has
	 * done these operations and cleared them.
	 */
	regmap_write(dev->map, DWC_IC_ENABLE(dev), 0);
	dev->status &= ~STATUS_ACTIVE;
}

static inline void __dwc_i2c_write_intr_mask(struct dwc_i2c_dev *dev,
					    unsigned int intr_mask)
{
	unsigned int val = dev->flags & ACCESS_POLLING ? 0 : intr_mask;

	regmap_write(dev->map, DWC_IC_INTR_MASK(dev), val);
	dev->sw_mask = intr_mask;
}

static inline void __dwc_i2c_read_intr_mask(struct dwc_i2c_dev *dev,
					   unsigned int *intr_mask)
{
	if (!(dev->flags & ACCESS_POLLING))
		regmap_read(dev->map, DWC_IC_INTR_MASK(dev), intr_mask);
	else
		*intr_mask = dev->sw_mask;
}

static inline void __dwc_i2c_write_smbus_intr_mask(struct dwc_i2c_dev *dev,
						   unsigned int intr_mask)
{
	unsigned int val = dev->flags & ACCESS_POLLING ? 0 : intr_mask;

	if (!dev->caps.smbus)
		return;

	regmap_write(dev->map, DWC_IC_SMBUS_INTR_MASK(dev), val);
	dev->sw_smbus_mask = intr_mask;
}

static inline void __dwc_i2c_read_smbus_intr_mask(struct dwc_i2c_dev *dev,
						  unsigned int *intr_mask)
{
	if (!dev->caps.smbus)
		*intr_mask = 0;
	else if (!(dev->flags & ACCESS_POLLING))
		regmap_read(dev->map, DWC_IC_SMBUS_INTR_MASK(dev), intr_mask);
	else
		*intr_mask = dev->sw_smbus_mask;
}

static inline bool __dwc_i2c_interrupts_masked(struct dwc_i2c_dev *dev)
{
	/*
	 * Use the software mask value because some mask register's fields that
	 * are unavailable by the configuration are initialized as 1 though they
	 * may be read-only.
	 */
	return !(dev->sw_mask || dev->sw_smbus_mask);
}

int dwc_i2c_terminate_by_polling(struct dwc_i2c_dev *dev);
void __dwc_i2c_disable(struct dwc_i2c_dev *dev);
void dwc_i2c_do_reset(struct dwc_i2c_dev *dev);

extern void dwc_i2c_configure_controller(struct dwc_i2c_dev *dev);
extern int dwc_i2c_probe_controller(struct dwc_i2c_dev *dev);

#if IS_ENABLED(CONFIG_I2C_DWC_TARGET)
extern void dwc_i2c_configure_target(struct dwc_i2c_dev *dev);
extern int dwc_i2c_probe_target(struct dwc_i2c_dev *dev);
#else
static inline void dwc_i2c_configure_target(struct dwc_i2c_dev *dev) { }
static inline int dwc_i2c_probe_target(struct dwc_i2c_dev *dev) { return -EINVAL; }
#endif

int dwc_i2c_probe(struct dwc_i2c_dev *dev);
