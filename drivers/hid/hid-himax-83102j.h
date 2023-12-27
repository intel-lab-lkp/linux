/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __HX_IC_83102J_H__
#define __HX_IC_83102J_H__

#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/acpi.h>
#include <linux/spi/spi.h>
#include <linux/hid.h>
#include <linux/sizes.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/types.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/proc_fs.h>
#include <linux/version.h>
#include <linux/firmware.h>
#include <linux/stddef.h>
#include <linux/power_supply.h>

#define HIMAX_BUS_RETRY_TIMES 3
// SPI bus read/write max length
#define HIMAX_BUS_RW_MAX_LEN 0x20006
// SPI bus read header length
#define HIMAX_BUS_R_HLEN 3
// SPI bus read data length, must be multiple of 4 and smaller than BUS_RW_MAX_LEN - BUS_R_HLEN
#define HIMAX_BUS_R_DLEN ((HIMAX_BUS_RW_MAX_LEN - HIMAX_BUS_R_HLEN) - ((HIMAX_BUS_RW_MAX_LEN - HIMAX_BUS_R_HLEN) % 4))
// SPI bus write header length
#define HIMAX_BUS_W_HLEN 2
// SPI bus write data length, must be multiple of 4 and smaller than BUS_RW_MAX_LEN - BUS_W_HLEN
#define HIMAX_BUS_W_DLEN ((HIMAX_BUS_RW_MAX_LEN - HIMAX_BUS_W_HLEN) - ((HIMAX_BUS_RW_MAX_LEN - HIMAX_BUS_W_HLEN) % 4))

enum HID_ID_FUNCT {
	ID_CONTACT_COUNT = 0x03,
};

enum HID_FW_UPDATE_STATUS_CODE {
	FWUP_ERROR_NO_ERROR = 0x77,
	FWUP_ERROR_NO_MAIN = 0xC2,
	FWUP_ERROR_BL_COMPLETE = 0xB1,
	FWUP_ERROR_BL = 0xB2,
	FWUP_ERROR_FLASH_PROGRAMMING = 0xB5,
};


// Register setting
#define HIMAX_REG_DATA_LEN			4
#define HIMAX_REG_ADDR_LEN			4
#define HIMAX_MAX_TRANS_SZ			128
#define HIMAX_MAX_RETRY_TIMES			5

#define HIMAX_HX83102J_STACK_SIZE			128
#define HIMAX_HX83102J_IC_ADR_TCON_RST     0x80020004
#define HIMAX_HX83102J_SAFE_MODE_PASSWORD			0x9527
#define HIMAX_HX83102J_ICID_ADDR					0x900000D0
#define HIMAX_HX83102J_ICID_DATA					0x83102900
#define HIMAX_HX83102J_MAX_RX_NUM			48
#define HIMAX_HX83102J_MAX_TX_NUM			32

#define HIMAX_IC_ADR_AHB_ADDR_BYTE_0           0x00
#define HIMAX_IC_ADR_AHB_RDATA_BYTE_0          0x08
#define HIMAX_IC_ADR_AHB_ACCESS_DIRECTION      0x0c
#define HIMAX_IC_ADR_CONTI                     0x13
#define HIMAX_IC_ADR_INCR4                     0x0D
#define HIMAX_IC_CMD_AHB_ACCESS_DIRECTION_READ 0x00
#define HIMAX_IC_CMD_CONTI                     0x31
#define HIMAX_IC_CMD_INCR4                     0x10
#define HIMAX_IC_ADR_CS_CENTRAL_STATE          0x900000A8

#define HIMAX_FW_ADDR_CTRL_FW                     0x9000005c
#define HIMAX_FW_USB_DETECT_ADDR                  0x10007F38
#define HIMAX_FW_DATA_SAFE_MODE_RELEASE_PW_RESET  0x00000000
#define HIMAX_FW_DATA_FW_STOP                     0x000000A5
#define HIMAX_FW_ADDR_AP_NOTIFY_FW_SUS            0x10007FD0
#define HIMAX_FW_DATA_AP_NOTIFY_FW_SUS_EN         0xA55AA55A
#define HIMAX_FW_DATA_AP_NOTIFY_FW_SUS_DIS        0x00000000
#define HIMAX_FW_ADDR_EVENT_ADDR                  0x30
#define HIMAX_FW_FUNC_HANDSHAKING_PWD             0xA55AA55A

#define HIMAX_FLASH_ADDR_CTRL_BASE           0x80000000
#define HIMAX_FLASH_ADDR_SPI200_DATA         (HIMAX_FLASH_ADDR_CTRL_BASE + 0x2c)

#define HIMAX_HID_REPORT_HDR_SZ (2)
#define HIMAX_HX83102J_ID		"HX83102J"


struct flash_version_info {
	u32 addr_hid_rd_desc;
};

struct himax_hid_rd_data_t {
	u8 *rd_data;
	u32 rd_length;
};
union himax_dword_data_t {
	u32 dword;
	u8 byte[4];
};

enum hid_reg_action {
	REG_READ = 0,
	REG_WRITE = 1
};

enum hid_reg_types {
	REG_TYPE_EXT_AHB,
	REG_TYPE_EXT_SRAM,
	REG_TYPE_EXT_TYPE = 0xFFFFFFFF
};
struct himax_hid_req_cfg_t {
	u32 data_type;
	u32 input_RD_de;
};

#define HIMAX_FULL_STACK_SIZE \
	(HIMAX_HX83102J_STACK_SIZE +\
	(2 + HIMAX_HX83102J_MAX_RX_NUM * HIMAX_HX83102J_MAX_TX_NUM + HIMAX_HX83102J_MAX_TX_NUM + HIMAX_HX83102J_MAX_RX_NUM)\
	* 2)

struct himax_ic_data {
	u32 HX_RX_NUM;
	u32 HX_TX_NUM;
	u32 HX_BT_NUM;
	u32 HX_MAX_PT;
	u8 HX_INT_IS_EDGE;
	u8 HX_STYLUS_FUNC;
	u8 HX_STYLUS_ID_V2;
	u8 HX_STYLUS_RATIO;
	u32 icid;
};

enum HX_TS_PATH {
	HIMAX_REPORT_COORD = 1,
};

enum HX_TS_STATUS {
	HIMAX_TS_GET_DATA_FAIL = -4,
	HIMAX_TS_NORMAL_END = 0,
};

struct himax_hid_desc_t {
	u16 desc_length;
	u16 bcd_version;
	u16 report_desc_length;
	u16 max_input_length;
	u16 max_output_length;
	u16 max_fragment_length;
	u16 vendor_id;
	u16 product_id;
	u16 version_id;
	u16 flags;
	u32 reserved;
} __packed;

struct himax_ts_data {
	bool initialized;
	bool probe_finish;
	bool suspended;
	char chip_name[30];
	bool ic_boot_done;
	u8 *xfer_data;
	struct himax_ic_data *ic_data;
	int touch_all_size;
	int touch_info_size;
	struct flash_version_info flash_ver_info;
	u8 irq_enabled;
	struct gpio_desc *gpiod_rst;
	s32 (*power)(s32 on);
	struct device *dev;
	struct himax_platform_data *pdata;
	/* mutex lock for reg access */
	struct mutex reg_lock;
	/* mutex lock for read/write action */
	struct mutex rw_lock;
	/* mutex lock for hid ioctl action */
	struct mutex hid_ioctl_lock;
	atomic_t irq_state;
	/* spin lock for irq */
	spinlock_t irq_lock;
	struct spi_device	*spi;
	s32 himax_irq;
	u8 *xfer_buff;
	struct hid_device *hid;
	struct himax_hid_desc_t hid_desc;
	struct himax_hid_rd_data_t hid_rd_data;
	bool hid_probe;
	struct delayed_work work_hid_update;
};

struct himax_platform_data {
	struct himax_ts_data *ts;
	struct gpio_desc *gpiod_rst;
};

#endif
