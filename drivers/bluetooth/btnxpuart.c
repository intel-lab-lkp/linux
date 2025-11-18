// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  NXP Bluetooth driver
 *  Copyright 2023-2025 NXP
 */

#include <linux/module.h>
#include <linux/kernel.h>

#include <linux/serdev.h>
#include <linux/of.h>
#include <linux/skbuff.h>
#include <linux/unaligned.h>
#include <linux/firmware.h>
#include <linux/string.h>
#include <linux/crc8.h>
#include <linux/crc32.h>
#include <linux/math.h>
#include <linux/string_helpers.h>
#include <linux/gpio/consumer.h>
#include <linux/of_irq.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>

#include <linux/crypto.h>
#include <crypto/sha2.h>
#include <crypto/hash.h>
#include <crypto/kpp.h>
#include <crypto/ecdh.h>
#include <linux/scatterlist.h>
#include <linux/completion.h>
#include <crypto/aes.h>
#include <crypto/gcm.h>
#include <crypto/aead.h>
#include <crypto/public_key.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "hci_uart.h"

#define MANUFACTURER_NXP		37

#define BTNXPUART_TX_STATE_ACTIVE	1
#define BTNXPUART_FW_DOWNLOADING	2
#define BTNXPUART_CHECK_BOOT_SIGNATURE	3
#define BTNXPUART_SERDEV_OPEN		4
#define BTNXPUART_IR_IN_PROGRESS	5
#define BTNXPUART_FW_DOWNLOAD_ABORT	6
#define BTNXPUART_FW_DUMP_IN_PROGRESS	7

/* NXP HW err codes */
#define BTNXPUART_IR_HW_ERR		0xb0

#define FIRMWARE_W8987		"uart8987_bt.bin"
#define FIRMWARE_W8987_OLD	"uartuart8987_bt.bin"
#define FIRMWARE_W8997		"uart8997_bt_v4.bin"
#define FIRMWARE_W8997_OLD	"uartuart8997_bt_v4.bin"
#define FIRMWARE_W9098		"uart9098_bt_v1.bin"
#define FIRMWARE_W9098_OLD	"uartuart9098_bt_v1.bin"
#define FIRMWARE_IW416		"uartiw416_bt.bin"
#define FIRMWARE_IW416_OLD	"uartiw416_bt_v0.bin"
#define FIRMWARE_IW612		"uartspi_n61x_v1.bin.se"
#define FIRMWARE_IW610		"uartspi_iw610.bin"
#define FIRMWARE_SECURE_IW610	"uartspi_iw610.bin.se"
#define FIRMWARE_IW624		"uartiw624_bt.bin"
#define FIRMWARE_SECURE_IW624	"uartiw624_bt.bin.se"
#define FIRMWARE_AW693		"uartaw693_bt.bin"
#define FIRMWARE_SECURE_AW693	"uartaw693_bt.bin.se"
#define FIRMWARE_AW693_A1		"uartaw693_bt_v1.bin"
#define FIRMWARE_SECURE_AW693_A1	"uartaw693_bt_v1.bin.se"
#define FIRMWARE_HELPER		"helper_uart_3000000.bin"

#define CHIP_ID_W9098		0x5c03
#define CHIP_ID_IW416		0x7201
#define CHIP_ID_IW612		0x7601
#define CHIP_ID_IW624a		0x8000
#define CHIP_ID_IW624c		0x8001
#define CHIP_ID_AW693a0		0x8200
#define CHIP_ID_AW693a1		0x8201
#define CHIP_ID_IW610a0		0x8800
#define CHIP_ID_IW610a1		0x8801

#define FW_SECURE_MASK		0xc0
#define FW_OPEN			0x00
#define FW_AUTH_ILLEGAL		0x40
#define FW_AUTH_PLAIN		0x80
#define FW_AUTH_ENC		0xc0

#define HCI_NXP_PRI_BAUDRATE	115200
#define HCI_NXP_SEC_BAUDRATE_3M	3000000
#define HCI_NXP_SEC_BAUDRATE_4M	4000000

#define MAX_FW_FILE_NAME_LEN    50

/* Default ps timeout period in milliseconds */
#define PS_DEFAULT_TIMEOUT_PERIOD_MS     2000

/* wakeup methods */
#define WAKEUP_METHOD_DTR       0
#define WAKEUP_METHOD_BREAK     1
#define WAKEUP_METHOD_EXT_BREAK 2
#define WAKEUP_METHOD_RTS       3
#define WAKEUP_METHOD_GPIO      4
#define WAKEUP_METHOD_INVALID   0xff

/* power save mode status */
#define PS_MODE_DISABLE         0
#define PS_MODE_ENABLE          1

/* Power Save Commands to ps_work_func  */
#define PS_CMD_EXIT_PS          1
#define PS_CMD_ENTER_PS         2

/* power save state */
#define PS_STATE_AWAKE          0
#define PS_STATE_SLEEP          1

/* NXP Vendor Commands. Refer user manual UM11628 on nxp.com */
/* Get FW version */
#define HCI_NXP_GET_FW_VERSION	0xfc0f
/* Set custom BD Address */
#define HCI_NXP_SET_BD_ADDR	0xfc22
/* Set Auto-Sleep mode */
#define HCI_NXP_AUTO_SLEEP_MODE	0xfc23
/* Set Wakeup method */
#define HCI_NXP_WAKEUP_METHOD	0xfc53
/* Set operational baudrate */
#define HCI_NXP_SET_OPER_SPEED	0xfc09
/* Independent Reset (Soft Reset) */
#define HCI_NXP_IND_RESET	0xfcfc
/* Bluetooth vendor command: Trigger FW dump */
#define HCI_NXP_TRIGGER_DUMP	0xfe91
/* Bluetooth vendor command: Secure Host Interface */
#define HCI_NXP_SHI_ENCRYPT	0xfe9c

/* Bluetooth Power State : Vendor cmd params */
#define BT_PS_ENABLE			0x02
#define BT_PS_DISABLE			0x03

/* Bluetooth Host Wakeup Methods */
#define BT_HOST_WAKEUP_METHOD_NONE      0x00
#define BT_HOST_WAKEUP_METHOD_DTR       0x01
#define BT_HOST_WAKEUP_METHOD_BREAK     0x02
#define BT_HOST_WAKEUP_METHOD_GPIO      0x03

/* Bluetooth Chip Wakeup Methods */
#define BT_CTRL_WAKEUP_METHOD_DSR       0x00
#define BT_CTRL_WAKEUP_METHOD_BREAK     0x01
#define BT_CTRL_WAKEUP_METHOD_GPIO      0x02
#define BT_CTRL_WAKEUP_METHOD_EXT_BREAK 0x04
#define BT_CTRL_WAKEUP_METHOD_RTS       0x05

/* FW Metadata */
#define FW_METADATA_TLV_UUID		0x40
#define FW_METADATA_TLV_ECDSA_KEY	0x50
#define FW_METADATA_FLAG_BT		0x02

#define NXP_FW_UUID_SIZE		16
#define NXP_FW_ECDH_PUBKEY_SIZE		64
#define NXP_FW_ECDSA_PUBKEY_SIZE	65

struct ps_data {
	u8    target_ps_mode;	/* ps mode to be set */
	u8    cur_psmode;	/* current ps_mode */
	u8    ps_state;		/* controller's power save state */
	u8    ps_cmd;
	u8    h2c_wakeupmode;
	u8    cur_h2c_wakeupmode;
	u8    c2h_wakeupmode;
	u8    c2h_wakeup_gpio;
	u8    h2c_wakeup_gpio;
	bool  driver_sent_cmd;
	u16   h2c_ps_interval;
	u16   c2h_ps_interval;
	bool  wakeup_source;
	struct gpio_desc *h2c_ps_gpio;
	s32 irq_handler;
	struct hci_dev *hdev;
	struct work_struct work;
	struct timer_list ps_timer;
	struct mutex ps_lock;
};

struct wakeup_cmd_payload {
	u8 c2h_wakeupmode;
	u8 c2h_wakeup_gpio;
	u8 h2c_wakeupmode;
	u8 h2c_wakeup_gpio;
} __packed;

struct psmode_cmd_payload {
	u8 ps_cmd;
	__le16 c2h_ps_interval;
} __packed;

struct btnxpuart_data {
	const char *helper_fw_name;
	const char *fw_name;
	const char *fw_name_old;
};

enum bootloader_param_change {
	not_changed,
	cmd_sent,
	changed
};

struct nxp_tls_traffic_keys {
	u8 h2d_secret[SHA256_DIGEST_SIZE];
	u8 d2h_secret[SHA256_DIGEST_SIZE];
	/* These keys below should be used for message encryption/decryption */
	u8 h2d_iv[GCM_AES_IV_SIZE];
	u8 h2d_key[AES_KEYSIZE_128];
	u8 d2h_iv[GCM_AES_IV_SIZE];
	u8 d2h_key[AES_KEYSIZE_128];
};

struct btnxpuart_crypto {
	struct crypto_shash *tls_handshake_hash_tfm;
	struct shash_desc *tls_handshake_hash_desc;
	struct crypto_kpp *kpp;
	u8 ecdh_public[NXP_FW_ECDH_PUBKEY_SIZE];	/* ECDH public key, Key negotiation */
	u8 ecdsa_public[NXP_FW_ECDSA_PUBKEY_SIZE];	/* ECDSA public key, Authentication*/
	u8 fw_uuid[NXP_FW_UUID_SIZE];
	u8 handshake_h2_hash[SHA256_DIGEST_SIZE];
	u8 handshake_secret[SHA256_DIGEST_SIZE];
	u8 master_secret[SHA256_DIGEST_SIZE];
	struct completion completion;
	int decrypt_result;
	struct nxp_tls_traffic_keys keys;
};

struct btnxpuart_dev {
	struct hci_dev *hdev;
	struct serdev_device *serdev;

	struct work_struct tx_work;
	unsigned long tx_state;
	struct sk_buff_head txq;
	struct sk_buff *rx_skb;

	const struct firmware *fw;
	u8 fw_name[MAX_FW_FILE_NAME_LEN];
	u32 fw_dnld_v1_offset;
	u32 fw_v1_sent_bytes;
	u32 fw_dnld_v3_offset;
	u32 fw_v3_offset_correction;
	u32 fw_v3_prev_sent;
	u32 fw_v1_expected_len;
	u32 boot_reg_offset;
	wait_queue_head_t fw_dnld_done_wait_q;
	wait_queue_head_t check_boot_sign_wait_q;

	u32 new_baudrate;
	u32 current_baudrate;
	u32 fw_init_baudrate;
	u32 secondary_baudrate;
	enum bootloader_param_change timeout_changed;
	enum bootloader_param_change baudrate_changed;
	bool helper_downloaded;

	struct ps_data psdata;
	struct btnxpuart_data *nxp_data;
	struct reset_control *pdn;
	struct hci_uart hu;
	bool secure_interface;
	struct btnxpuart_crypto crypto;
};

#define NXP_V1_FW_REQ_PKT	0xa5
#define NXP_V1_CHIP_VER_PKT	0xaa
#define NXP_V3_FW_REQ_PKT	0xa7
#define NXP_V3_CHIP_VER_PKT	0xab

#define NXP_ACK_V1		0x5a
#define NXP_NAK_V1		0xbf
#define NXP_ACK_V3		0x7a
#define NXP_NAK_V3		0x7b
#define NXP_CRC_ERROR_V3	0x7c

/* Bootloader signature error codes: Refer AN12820 from nxp.com */
#define NXP_CRC_RX_ERROR	BIT(0)	/* CRC error in previous packet */
#define NXP_ACK_RX_TIMEOUT	BIT(2)	/* ACK not received from host */
#define NXP_HDR_RX_TIMEOUT	BIT(3)	/* FW Header chunk not received */
#define NXP_DATA_RX_TIMEOUT	BIT(4)	/* FW Data chunk not received */

#define HDR_LEN			16

#define NXP_RECV_CHIP_VER_V1 \
	.type = NXP_V1_CHIP_VER_PKT, \
	.hlen = 4, \
	.loff = 0, \
	.lsize = 0, \
	.maxlen = 4

#define NXP_RECV_FW_REQ_V1 \
	.type = NXP_V1_FW_REQ_PKT, \
	.hlen = 4, \
	.loff = 0, \
	.lsize = 0, \
	.maxlen = 4

#define NXP_RECV_CHIP_VER_V3 \
	.type = NXP_V3_CHIP_VER_PKT, \
	.hlen = 4, \
	.loff = 0, \
	.lsize = 0, \
	.maxlen = 4

#define NXP_RECV_FW_REQ_V3 \
	.type = NXP_V3_FW_REQ_PKT, \
	.hlen = 9, \
	.loff = 0, \
	.lsize = 0, \
	.maxlen = 9

struct v1_data_req {
	__le16 len;
	__le16 len_comp;
} __packed;

struct v1_start_ind {
	__le16 chip_id;
	__le16 chip_id_comp;
} __packed;

struct v3_data_req {
	__le16 len;
	__le32 offset;
	__le16 error;
	u8 crc;
} __packed;

struct v3_start_ind {
	__le16 chip_id;
	u8 loader_ver;
	u8 crc;
} __packed;

/* UART register addresses of BT chip */
#define CLKDIVADDR	0x7f00008f
#define UARTDIVADDR	0x7f000090
#define UARTMCRADDR	0x7f000091
#define UARTREINITADDR	0x7f000092
#define UARTICRADDR	0x7f000093
#define UARTFCRADDR	0x7f000094

#define MCR		0x00000022
#define INIT		0x00000001
#define ICR		0x000000c7
#define FCR		0x000000c7

#define POLYNOMIAL8	0x07

struct uart_reg {
	__le32 address;
	__le32 value;
} __packed;

struct uart_config {
	struct uart_reg clkdiv;
	struct uart_reg uartdiv;
	struct uart_reg mcr;
	struct uart_reg re_init;
	struct uart_reg icr;
	struct uart_reg fcr;
	__be32 crc;
} __packed;

struct nxp_bootloader_cmd {
	__le32 header;
	__le32 arg;
	__le32 payload_len;
	__be32 crc;
} __packed;

struct nxp_v3_rx_timeout_nak {
	u8 nak;
	__le32 offset;
	u8 crc;
} __packed;

union nxp_v3_rx_timeout_nak_u {
	struct nxp_v3_rx_timeout_nak pkt;
	u8 buf[6];
};

struct nxp_v3_crc_nak {
	u8 nak;
	u8 crc;
} __packed;

union nxp_v3_crc_nak_u {
	struct nxp_v3_crc_nak pkt;
	u8 buf[2];
};

/* FW dump */
#define NXP_FW_DUMP_SIZE	(1024 * 1000)

struct nxp_fw_dump_hdr {
	__le16 seq_num;
	__le16 reserved;
	__le16 buf_type;
	__le16 buf_len;
};

union nxp_set_bd_addr_payload {
	struct {
		u8 param_id;
		u8 param_len;
		u8 param[6];
	} __packed data;
	u8 buf[8];
};

/* Secure Host Interface */
#define NXP_TLS_MAGIC			0x43b826f3
#define NXP_TLS_VERSION			1

#define NXP_TLS_ECDH_PUBLIC_KEY_SIZE	64
#define NXP_DEVICE_UUID_LEN		16
#define NXP_ENC_AUTH_TAG_SIZE		16

#define NXP_TLS_LABEL(str)		str, strlen(str)
#define NXP_TLS_DEVICE_HS_TS_LABEL	NXP_TLS_LABEL("D HS TS")
#define NXP_TLS_KEYING_IV_LABEL		NXP_TLS_LABEL("iv")
#define NXP_TLS_KEYING_KEY_LABEL	NXP_TLS_LABEL("key")
#define NXP_TLS_FINISHED_LABEL		NXP_TLS_LABEL("finished")
#define NXP_TLS_DERIVED_LABEL		NXP_TLS_LABEL("derived")
#define NXP_TLS_HOST_HS_TS_LABEL	NXP_TLS_LABEL("H HS TS")
#define NXP_TLS_D_AP_TS_LABEL		NXP_TLS_LABEL("D AP TS")
#define NXP_TLS_H_AP_TS_LABEL		NXP_TLS_LABEL("H AP TS")

enum nxp_tls_signature_algorithm {
	NXP_TLS_ECDSA_SECP256R1_SHA256 = 0x0403,
};

enum nxp_tls_key_exchange_type {
	NXP_TLS_ECDHE_SECP256R1 = 0x0017,
};

enum nxp_tls_cipher_suite {
	NXP_TLS_AES_128_GCM_SHA256 = 0x1301,
};

enum nxp_tls_message_id {
	NXP_TLS_HOST_HELLO	= 1,
	NXP_TLS_DEVICE_HELLO	= 2,
	NXP_TLS_HOST_FINISHED	= 3,
};

struct nxp_tls_message_hdr {
	__le32 magic;
	__le16 len;
	u8 message_id;
	u8 protocol_version;
};

struct nxp_tls_host_hello {
	struct nxp_tls_message_hdr hdr;
	__le16 sig_alg;
	__le16 key_exchange_type;
	__le16 cipher_suite;
	__le16 reserved;
	u8 random[32];
	u8 pubkey[NXP_TLS_ECDH_PUBLIC_KEY_SIZE]; /* ECDHE */
};

union nxp_tls_host_hello_payload {
	struct {
		u8 msg_type;
		struct nxp_tls_host_hello host_hello;
	} __packed;
	u8 buf[113];
};

struct nxp_tls_device_info {
	__le16 chip_id;
	__le16 device_flags;
	u8 reserved[4];
	u8 uuid[NXP_DEVICE_UUID_LEN];
};

struct nxp_tls_signature {
	u8 sig[64];        /* P-256 ECDSA signature, two points */
};

struct nxp_tls_finished {
	u8 verify_data[32];
};

struct nxp_tls_device_hello {
	struct nxp_tls_message_hdr hdr;
	__le32 reserved;
	u8 random[32];
	u8 pubkey[NXP_TLS_ECDH_PUBLIC_KEY_SIZE];
	/* Encrypted portion */
	struct {
		struct nxp_tls_device_info device_info;
		struct nxp_tls_signature device_handshake_sig;   /* TLS Certificate Verify */
		struct nxp_tls_finished device_finished;
	} enc;
	u8 auth_tag[NXP_ENC_AUTH_TAG_SIZE];   /* Auth tag for the encrypted portion */
};

struct nxp_tls_data_add {
	u8 version;        /* NXP_TLS_VERSION */
	u8 reserved[5];    /* zeroes */
	__le16 len;
};

struct nxp_tls_host_finished {
	struct nxp_tls_message_hdr hdr;
	__le32 reserved;
	/* Encrypted portion */
	struct {
		struct nxp_tls_signature reserved2;
		struct nxp_tls_finished host_finished;
	} enc;
	u8 auth_tag[NXP_ENC_AUTH_TAG_SIZE];   /* Auth tag for the encrypted portion */
};

union nxp_tls_host_finished_payload {
	struct {
		u8 msg_type;
		struct nxp_tls_host_finished host_finished;
	} __packed;
	u8 buf[125];
};

#define DEVICE_HELLO_SIG_CUTOFF_POS \
	offsetof(struct nxp_tls_device_hello, enc)

#define DEVICE_HELLO_FINISHED_ENC_CUTOFF_POS \
	(offsetof(struct nxp_tls_device_hello, enc.device_finished) - \
	DEVICE_HELLO_SIG_CUTOFF_POS)


#define HOST_FINISHED_CUTOFF_POS \
	offsetof(struct nxp_tls_host_finished, enc.host_finished)

/* FW Meta Data */
struct fw_metadata_hdr {
	__le32 cmd;
	__le32 addr;
	__le32 len;
	__le32 crc;
};

struct fw_metadata_tail {
	__le32 len;
	u8 magic[8];
	__le32 crc;
};

struct fw_metadata_tlv {
	__le16 id;
	__le16 flag;
	__le32 len;
};

static u8 crc8_table[CRC8_TABLE_SIZE];

/* Default configurations */
#define DEFAULT_H2C_WAKEUP_MODE	WAKEUP_METHOD_BREAK
#define DEFAULT_PS_MODE		PS_MODE_ENABLE
#define FW_INIT_BAUDRATE	HCI_NXP_PRI_BAUDRATE

static struct sk_buff *nxp_drv_send_cmd(struct hci_dev *hdev, u16 opcode,
					u32 plen,
					void *param,
					bool resp)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	struct ps_data *psdata = &nxpdev->psdata;
	struct sk_buff *skb = NULL;

	/* set flag to prevent nxp_enqueue from parsing values from this command and
	 * calling hci_cmd_sync_queue() again.
	 */
	psdata->driver_sent_cmd = true;
	if (resp) {
		skb = __hci_cmd_sync(hdev, opcode, plen, param, HCI_CMD_TIMEOUT);
	} else {
		__hci_cmd_send(hdev, opcode, plen, param);
		/* Allow command to be sent before tx_work is cancelled
		 * by btnxpuart_flush()
		 */
		msleep(20);
	}
	psdata->driver_sent_cmd = false;

	return skb;
}

static void btnxpuart_tx_wakeup(struct btnxpuart_dev *nxpdev)
{
	if (schedule_work(&nxpdev->tx_work))
		set_bit(BTNXPUART_TX_STATE_ACTIVE, &nxpdev->tx_state);
}

/* NXP Power Save Feature */
static void ps_start_timer(struct btnxpuart_dev *nxpdev)
{
	struct ps_data *psdata = &nxpdev->psdata;

	if (!psdata)
		return;

	if (psdata->cur_psmode == PS_MODE_ENABLE)
		mod_timer(&psdata->ps_timer, jiffies + msecs_to_jiffies(psdata->h2c_ps_interval));

	if (psdata->ps_state == PS_STATE_AWAKE && psdata->ps_cmd == PS_CMD_ENTER_PS)
		cancel_work_sync(&psdata->work);
}

static void ps_cancel_timer(struct btnxpuart_dev *nxpdev)
{
	struct ps_data *psdata = &nxpdev->psdata;

	flush_work(&psdata->work);
	timer_shutdown_sync(&psdata->ps_timer);
}

static void ps_control(struct hci_dev *hdev, u8 ps_state)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	struct ps_data *psdata = &nxpdev->psdata;
	int status = 0;

	if (psdata->ps_state == ps_state ||
	    !test_bit(BTNXPUART_SERDEV_OPEN, &nxpdev->tx_state))
		return;

	mutex_lock(&psdata->ps_lock);
	switch (psdata->cur_h2c_wakeupmode) {
	case WAKEUP_METHOD_GPIO:
		if (ps_state == PS_STATE_AWAKE)
			gpiod_set_value_cansleep(psdata->h2c_ps_gpio, 0);
		else
			gpiod_set_value_cansleep(psdata->h2c_ps_gpio, 1);
		bt_dev_dbg(hdev, "Set h2c_ps_gpio: %s",
			   str_high_low(ps_state == PS_STATE_SLEEP));
		break;
	case WAKEUP_METHOD_DTR:
		if (ps_state == PS_STATE_AWAKE)
			status = serdev_device_set_tiocm(nxpdev->serdev, TIOCM_DTR, 0);
		else
			status = serdev_device_set_tiocm(nxpdev->serdev, 0, TIOCM_DTR);
		break;
	case WAKEUP_METHOD_BREAK:
	default:
		if (ps_state == PS_STATE_AWAKE)
			status = serdev_device_break_ctl(nxpdev->serdev, 0);
		else
			status = serdev_device_break_ctl(nxpdev->serdev, -1);
		msleep(20); /* Allow chip to detect UART-break and enter sleep */
		bt_dev_dbg(hdev, "Set UART break: %s, status=%d",
			   str_on_off(ps_state == PS_STATE_SLEEP), status);
		break;
	}
	if (!status)
		psdata->ps_state = ps_state;
	mutex_unlock(&psdata->ps_lock);

	if (ps_state == PS_STATE_AWAKE)
		btnxpuart_tx_wakeup(nxpdev);
}

static void ps_work_func(struct work_struct *work)
{
	struct ps_data *data = container_of(work, struct ps_data, work);

	if (data->ps_cmd == PS_CMD_ENTER_PS && data->cur_psmode == PS_MODE_ENABLE)
		ps_control(data->hdev, PS_STATE_SLEEP);
	else if (data->ps_cmd == PS_CMD_EXIT_PS)
		ps_control(data->hdev, PS_STATE_AWAKE);
}

static void ps_timeout_func(struct timer_list *t)
{
	struct ps_data *data = timer_container_of(data, t, ps_timer);
	struct hci_dev *hdev = data->hdev;
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);

	if (test_bit(BTNXPUART_TX_STATE_ACTIVE, &nxpdev->tx_state)) {
		ps_start_timer(nxpdev);
	} else {
		data->ps_cmd = PS_CMD_ENTER_PS;
		schedule_work(&data->work);
	}
}

static irqreturn_t ps_host_wakeup_irq_handler(int irq, void *priv)
{
	struct btnxpuart_dev *nxpdev = (struct btnxpuart_dev *)priv;

	bt_dev_dbg(nxpdev->hdev, "Host wakeup interrupt");
	return IRQ_HANDLED;
}
static int ps_setup(struct hci_dev *hdev)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	struct serdev_device *serdev = nxpdev->serdev;
	struct ps_data *psdata = &nxpdev->psdata;
	int ret;

	/* Out-Of-Band Device Wakeup */
	psdata->h2c_ps_gpio = devm_gpiod_get_optional(&serdev->dev, "device-wakeup",
						      GPIOD_OUT_LOW);
	if (IS_ERR(psdata->h2c_ps_gpio)) {
		bt_dev_err(hdev, "Error fetching device-wakeup-gpios: %ld",
			   PTR_ERR(psdata->h2c_ps_gpio));
		return PTR_ERR(psdata->h2c_ps_gpio);
	}

	if (device_property_read_u8(&serdev->dev, "nxp,wakein-pin", &psdata->h2c_wakeup_gpio)) {
		psdata->h2c_wakeup_gpio = 0xff; /* 0xff: use default pin/gpio */
	} else if (!psdata->h2c_ps_gpio) {
		bt_dev_warn(hdev, "nxp,wakein-pin property without device-wakeup-gpios");
		psdata->h2c_wakeup_gpio = 0xff;
	}

	/* Out-Of-Band Host Wakeup */
	if (of_property_read_bool(serdev->dev.of_node, "wakeup-source")) {
		psdata->irq_handler = of_irq_get_byname(serdev->dev.of_node, "wakeup");
		bt_dev_info(nxpdev->hdev, "irq_handler: %d", psdata->irq_handler);
		if (psdata->irq_handler > 0)
			psdata->wakeup_source = true;
	}

	if (device_property_read_u8(&serdev->dev, "nxp,wakeout-pin", &psdata->c2h_wakeup_gpio)) {
		psdata->c2h_wakeup_gpio = 0xff;
		if (psdata->wakeup_source) {
			bt_dev_warn(hdev, "host wakeup interrupt without nxp,wakeout-pin");
			psdata->wakeup_source = false;
		}
	} else if (!psdata->wakeup_source) {
		bt_dev_warn(hdev, "nxp,wakeout-pin property without host wakeup interrupt");
		psdata->c2h_wakeup_gpio = 0xff;
	}

	if (psdata->wakeup_source) {
		ret = devm_request_threaded_irq(&serdev->dev, psdata->irq_handler,
						NULL, ps_host_wakeup_irq_handler,
						IRQF_ONESHOT,
						dev_name(&serdev->dev), nxpdev);
		if (ret)
			bt_dev_info(hdev, "error setting wakeup IRQ handler, ignoring\n");
		disable_irq(psdata->irq_handler);
		device_init_wakeup(&serdev->dev, true);
	}

	psdata->hdev = hdev;
	INIT_WORK(&psdata->work, ps_work_func);
	mutex_init(&psdata->ps_lock);
	timer_setup(&psdata->ps_timer, ps_timeout_func, 0);

	return 0;
}

static bool ps_wakeup(struct btnxpuart_dev *nxpdev)
{
	struct ps_data *psdata = &nxpdev->psdata;
	u8 ps_state;

	mutex_lock(&psdata->ps_lock);
	ps_state = psdata->ps_state;
	mutex_unlock(&psdata->ps_lock);

	if (ps_state != PS_STATE_AWAKE) {
		psdata->ps_cmd = PS_CMD_EXIT_PS;
		schedule_work(&psdata->work);
		return true;
	}
	return false;
}

static void ps_cleanup(struct btnxpuart_dev *nxpdev)
{
	struct ps_data *psdata = &nxpdev->psdata;
	u8 ps_state;

	mutex_lock(&psdata->ps_lock);
	ps_state = psdata->ps_state;
	mutex_unlock(&psdata->ps_lock);

	if (ps_state != PS_STATE_AWAKE)
		ps_control(psdata->hdev, PS_STATE_AWAKE);

	ps_cancel_timer(nxpdev);
	cancel_work_sync(&psdata->work);
	mutex_destroy(&psdata->ps_lock);
}

static int send_ps_cmd(struct hci_dev *hdev, void *data)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	struct ps_data *psdata = &nxpdev->psdata;
	struct psmode_cmd_payload pcmd;
	struct sk_buff *skb;
	u8 *status;

	if (psdata->target_ps_mode == PS_MODE_ENABLE)
		pcmd.ps_cmd = BT_PS_ENABLE;
	else
		pcmd.ps_cmd = BT_PS_DISABLE;
	pcmd.c2h_ps_interval = __cpu_to_le16(psdata->c2h_ps_interval);

	skb = nxp_drv_send_cmd(hdev, HCI_NXP_AUTO_SLEEP_MODE, sizeof(pcmd),
			       &pcmd, true);
	if (IS_ERR(skb)) {
		bt_dev_err(hdev, "Setting Power Save mode failed (%ld)", PTR_ERR(skb));
		return PTR_ERR(skb);
	}

	status = skb_pull_data(skb, 1);
	if (status) {
		if (!*status)
			psdata->cur_psmode = psdata->target_ps_mode;
		else
			psdata->target_ps_mode = psdata->cur_psmode;
		if (psdata->cur_psmode == PS_MODE_ENABLE)
			ps_start_timer(nxpdev);
		else
			ps_wakeup(nxpdev);
		bt_dev_dbg(hdev, "Power Save mode response: status=%d, ps_mode=%d",
			   *status, psdata->cur_psmode);
	}
	kfree_skb(skb);

	return 0;
}

static int send_wakeup_method_cmd(struct hci_dev *hdev, void *data)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	struct ps_data *psdata = &nxpdev->psdata;
	struct wakeup_cmd_payload pcmd;
	struct sk_buff *skb;
	u8 *status;

	pcmd.c2h_wakeupmode = psdata->c2h_wakeupmode;
	pcmd.c2h_wakeup_gpio = psdata->c2h_wakeup_gpio;
	pcmd.h2c_wakeup_gpio = 0xff;
	switch (psdata->h2c_wakeupmode) {
	case WAKEUP_METHOD_GPIO:
		pcmd.h2c_wakeupmode = BT_CTRL_WAKEUP_METHOD_GPIO;
		pcmd.h2c_wakeup_gpio = psdata->h2c_wakeup_gpio;
		break;
	case WAKEUP_METHOD_DTR:
		pcmd.h2c_wakeupmode = BT_CTRL_WAKEUP_METHOD_DSR;
		break;
	case WAKEUP_METHOD_BREAK:
	default:
		pcmd.h2c_wakeupmode = BT_CTRL_WAKEUP_METHOD_BREAK;
		break;
	}

	skb = nxp_drv_send_cmd(hdev, HCI_NXP_WAKEUP_METHOD, sizeof(pcmd),
			       &pcmd, true);
	if (IS_ERR(skb)) {
		bt_dev_err(hdev, "Setting wake-up method failed (%ld)", PTR_ERR(skb));
		return PTR_ERR(skb);
	}

	status = skb_pull_data(skb, 1);
	if (status) {
		if (*status == 0)
			psdata->cur_h2c_wakeupmode = psdata->h2c_wakeupmode;
		else
			psdata->h2c_wakeupmode = psdata->cur_h2c_wakeupmode;
		bt_dev_dbg(hdev, "Set Wakeup Method response: status=%d, h2c_wakeupmode=%d",
			   *status, psdata->cur_h2c_wakeupmode);
	}
	kfree_skb(skb);

	return 0;
}

static void ps_init(struct hci_dev *hdev)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	struct ps_data *psdata = &nxpdev->psdata;
	u8 default_h2c_wakeup_mode = DEFAULT_H2C_WAKEUP_MODE;

	serdev_device_set_tiocm(nxpdev->serdev, 0, TIOCM_RTS);
	usleep_range(5000, 10000);
	serdev_device_set_tiocm(nxpdev->serdev, TIOCM_RTS, 0);
	usleep_range(5000, 10000);

	psdata->ps_state = PS_STATE_AWAKE;

	if (psdata->c2h_wakeup_gpio != 0xff)
		psdata->c2h_wakeupmode = BT_HOST_WAKEUP_METHOD_GPIO;
	else
		psdata->c2h_wakeupmode = BT_HOST_WAKEUP_METHOD_NONE;

	psdata->cur_h2c_wakeupmode = WAKEUP_METHOD_INVALID;
	if (psdata->h2c_ps_gpio)
		default_h2c_wakeup_mode = WAKEUP_METHOD_GPIO;

	psdata->h2c_ps_interval = PS_DEFAULT_TIMEOUT_PERIOD_MS;

	switch (default_h2c_wakeup_mode) {
	case WAKEUP_METHOD_GPIO:
		psdata->h2c_wakeupmode = WAKEUP_METHOD_GPIO;
		gpiod_set_value_cansleep(psdata->h2c_ps_gpio, 0);
		usleep_range(5000, 10000);
		break;
	case WAKEUP_METHOD_DTR:
		psdata->h2c_wakeupmode = WAKEUP_METHOD_DTR;
		serdev_device_set_tiocm(nxpdev->serdev, 0, TIOCM_DTR);
		serdev_device_set_tiocm(nxpdev->serdev, TIOCM_DTR, 0);
		break;
	case WAKEUP_METHOD_BREAK:
	default:
		psdata->h2c_wakeupmode = WAKEUP_METHOD_BREAK;
		serdev_device_break_ctl(nxpdev->serdev, -1);
		usleep_range(5000, 10000);
		serdev_device_break_ctl(nxpdev->serdev, 0);
		usleep_range(5000, 10000);
		break;
	}

	psdata->cur_psmode = PS_MODE_DISABLE;
	psdata->target_ps_mode = DEFAULT_PS_MODE;
}

/* NXP Firmware Download Feature */
static int nxp_download_firmware(struct hci_dev *hdev)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	int err = 0;

	nxpdev->fw_dnld_v1_offset = 0;
	nxpdev->fw_v1_sent_bytes = 0;
	nxpdev->fw_v1_expected_len = HDR_LEN;
	nxpdev->boot_reg_offset = 0;
	nxpdev->fw_dnld_v3_offset = 0;
	nxpdev->fw_v3_offset_correction = 0;
	nxpdev->baudrate_changed = not_changed;
	nxpdev->timeout_changed = not_changed;
	nxpdev->helper_downloaded = false;

	serdev_device_set_baudrate(nxpdev->serdev, HCI_NXP_PRI_BAUDRATE);
	serdev_device_set_flow_control(nxpdev->serdev, false);
	nxpdev->current_baudrate = HCI_NXP_PRI_BAUDRATE;

	/* Wait till FW is downloaded */
	err = wait_event_interruptible_timeout(nxpdev->fw_dnld_done_wait_q,
					       !test_bit(BTNXPUART_FW_DOWNLOADING,
							 &nxpdev->tx_state),
					       msecs_to_jiffies(60000));

	if (nxpdev->fw && strlen(nxpdev->fw_name)) {
		release_firmware(nxpdev->fw);
		memset(nxpdev->fw_name, 0, sizeof(nxpdev->fw_name));
	}

	if (err == 0) {
		bt_dev_err(hdev, "FW Download Timeout. offset: %d",
				nxpdev->fw_dnld_v1_offset ?
				nxpdev->fw_dnld_v1_offset :
				nxpdev->fw_dnld_v3_offset);
		return -ETIMEDOUT;
	}
	if (test_bit(BTNXPUART_FW_DOWNLOAD_ABORT, &nxpdev->tx_state)) {
		bt_dev_err(hdev, "FW Download Aborted");
		return -EINTR;
	}

	serdev_device_set_flow_control(nxpdev->serdev, true);

	/* Allow the downloaded FW to initialize */
	msleep(1200);

	return 0;
}

static void nxp_send_ack(u8 ack, struct hci_dev *hdev)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	u8 ack_nak[2];
	int len = 1;

	ack_nak[0] = ack;
	if (ack == NXP_ACK_V3) {
		ack_nak[1] = crc8(crc8_table, ack_nak, 1, 0xff);
		len = 2;
	}
	serdev_device_write_buf(nxpdev->serdev, ack_nak, len);
}

static bool nxp_fw_change_baudrate(struct hci_dev *hdev, u16 req_len)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	struct nxp_bootloader_cmd nxp_cmd5;
	struct uart_config uart_config;
	u32 clkdivaddr = CLKDIVADDR - nxpdev->boot_reg_offset;
	u32 uartdivaddr = UARTDIVADDR - nxpdev->boot_reg_offset;
	u32 uartmcraddr = UARTMCRADDR - nxpdev->boot_reg_offset;
	u32 uartreinitaddr = UARTREINITADDR - nxpdev->boot_reg_offset;
	u32 uarticraddr = UARTICRADDR - nxpdev->boot_reg_offset;
	u32 uartfcraddr = UARTFCRADDR - nxpdev->boot_reg_offset;

	if (req_len == sizeof(nxp_cmd5)) {
		nxp_cmd5.header = __cpu_to_le32(5);
		nxp_cmd5.arg = 0;
		nxp_cmd5.payload_len = __cpu_to_le32(sizeof(uart_config));
		/* FW expects swapped CRC bytes */
		nxp_cmd5.crc = __cpu_to_be32(crc32_be(0UL, (char *)&nxp_cmd5,
						      sizeof(nxp_cmd5) - 4));

		serdev_device_write_buf(nxpdev->serdev, (u8 *)&nxp_cmd5, sizeof(nxp_cmd5));
		nxpdev->fw_v3_offset_correction += req_len;
	} else if (req_len == sizeof(uart_config)) {
		uart_config.clkdiv.address = __cpu_to_le32(clkdivaddr);
		if (nxpdev->new_baudrate == HCI_NXP_SEC_BAUDRATE_4M)
			uart_config.clkdiv.value = __cpu_to_le32(0x01000000);
		else
			uart_config.clkdiv.value = __cpu_to_le32(0x00c00000);
		uart_config.uartdiv.address = __cpu_to_le32(uartdivaddr);
		uart_config.uartdiv.value = __cpu_to_le32(1);
		uart_config.mcr.address = __cpu_to_le32(uartmcraddr);
		uart_config.mcr.value = __cpu_to_le32(MCR);
		uart_config.re_init.address = __cpu_to_le32(uartreinitaddr);
		uart_config.re_init.value = __cpu_to_le32(INIT);
		uart_config.icr.address = __cpu_to_le32(uarticraddr);
		uart_config.icr.value = __cpu_to_le32(ICR);
		uart_config.fcr.address = __cpu_to_le32(uartfcraddr);
		uart_config.fcr.value = __cpu_to_le32(FCR);
		/* FW expects swapped CRC bytes */
		uart_config.crc = __cpu_to_be32(crc32_be(0UL, (char *)&uart_config,
							 sizeof(uart_config) - 4));

		serdev_device_write_buf(nxpdev->serdev, (u8 *)&uart_config, sizeof(uart_config));
		serdev_device_wait_until_sent(nxpdev->serdev, 0);
		nxpdev->fw_v3_offset_correction += req_len;
		return true;
	}
	return false;
}

static bool nxp_fw_change_timeout(struct hci_dev *hdev, u16 req_len)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	struct nxp_bootloader_cmd nxp_cmd7;

	if (req_len != sizeof(nxp_cmd7))
		return false;

	nxp_cmd7.header = __cpu_to_le32(7);
	nxp_cmd7.arg = __cpu_to_le32(0x70);
	nxp_cmd7.payload_len = 0;
	/* FW expects swapped CRC bytes */
	nxp_cmd7.crc = __cpu_to_be32(crc32_be(0UL, (char *)&nxp_cmd7,
					      sizeof(nxp_cmd7) - 4));
	serdev_device_write_buf(nxpdev->serdev, (u8 *)&nxp_cmd7, sizeof(nxp_cmd7));
	serdev_device_wait_until_sent(nxpdev->serdev, 0);
	nxpdev->fw_v3_offset_correction += req_len;
	return true;
}

static u32 nxp_get_data_len(const u8 *buf)
{
	struct nxp_bootloader_cmd *hdr = (struct nxp_bootloader_cmd *)buf;

	return __le32_to_cpu(hdr->payload_len);
}

static bool is_fw_downloading(struct btnxpuart_dev *nxpdev)
{
	return test_bit(BTNXPUART_FW_DOWNLOADING, &nxpdev->tx_state);
}

static bool ind_reset_in_progress(struct btnxpuart_dev *nxpdev)
{
	return test_bit(BTNXPUART_IR_IN_PROGRESS, &nxpdev->tx_state);
}

static bool fw_dump_in_progress(struct btnxpuart_dev *nxpdev)
{
	return test_bit(BTNXPUART_FW_DUMP_IN_PROGRESS, &nxpdev->tx_state);
}

static bool process_boot_signature(struct btnxpuart_dev *nxpdev)
{
	if (test_bit(BTNXPUART_CHECK_BOOT_SIGNATURE, &nxpdev->tx_state)) {
		clear_bit(BTNXPUART_CHECK_BOOT_SIGNATURE, &nxpdev->tx_state);
		wake_up_interruptible(&nxpdev->check_boot_sign_wait_q);
		return false;
	}
	return is_fw_downloading(nxpdev);
}

static int nxp_request_firmware(struct hci_dev *hdev, const char *fw_name,
				const char *fw_name_old)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	const char *fw_name_dt;
	int err = 0;

	if (!fw_name)
		return -ENOENT;

	if (!strlen(nxpdev->fw_name)) {
		if (strcmp(fw_name, FIRMWARE_HELPER) &&
		    !device_property_read_string(&nxpdev->serdev->dev,
						 "firmware-name",
						 &fw_name_dt))
			fw_name = fw_name_dt;
		snprintf(nxpdev->fw_name, MAX_FW_FILE_NAME_LEN, "nxp/%s", fw_name);
		err = request_firmware_direct(&nxpdev->fw, nxpdev->fw_name, &hdev->dev);
		if (err < 0 && fw_name_old) {
			snprintf(nxpdev->fw_name, MAX_FW_FILE_NAME_LEN, "nxp/%s", fw_name_old);
			err = request_firmware_direct(&nxpdev->fw, nxpdev->fw_name, &hdev->dev);
		}

		bt_dev_info(hdev, "Request Firmware: %s", nxpdev->fw_name);
		if (err < 0) {
			bt_dev_err(hdev, "Firmware file %s not found", nxpdev->fw_name);
			clear_bit(BTNXPUART_FW_DOWNLOADING, &nxpdev->tx_state);
		}
	}
	return err;
}

/* for legacy chipsets with V1 bootloader */
static int nxp_recv_chip_ver_v1(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	struct v1_start_ind *req;
	__u16 chip_id;

	req = skb_pull_data(skb, sizeof(*req));
	if (!req)
		goto free_skb;

	chip_id = le16_to_cpu(req->chip_id ^ req->chip_id_comp);
	if (chip_id == 0xffff && nxpdev->fw_dnld_v1_offset) {
		nxpdev->fw_dnld_v1_offset = 0;
		nxpdev->fw_v1_sent_bytes = 0;
		nxpdev->fw_v1_expected_len = HDR_LEN;
		release_firmware(nxpdev->fw);
		memset(nxpdev->fw_name, 0, sizeof(nxpdev->fw_name));
		nxp_send_ack(NXP_ACK_V1, hdev);
	}

free_skb:
	kfree_skb(skb);
	return 0;
}

static int nxp_recv_fw_req_v1(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	struct btnxpuart_data *nxp_data = nxpdev->nxp_data;
	struct v1_data_req *req;
	__u16 len;

	if (!process_boot_signature(nxpdev))
		goto free_skb;

	req = skb_pull_data(skb, sizeof(*req));
	if (!req)
		goto free_skb;

	len = __le16_to_cpu(req->len ^ req->len_comp);
	if (len != 0xffff) {
		bt_dev_dbg(hdev, "ERR: Send NAK");
		nxp_send_ack(NXP_NAK_V1, hdev);
		goto free_skb;
	}
	nxp_send_ack(NXP_ACK_V1, hdev);

	len = __le16_to_cpu(req->len);

	if (!nxp_data->helper_fw_name) {
		if (nxpdev->timeout_changed != changed) {
			nxp_fw_change_timeout(hdev, len);
			nxpdev->timeout_changed = changed;
			goto free_skb;
		}
		if (nxpdev->baudrate_changed != changed) {
			nxpdev->new_baudrate = nxpdev->secondary_baudrate;
			if (nxp_fw_change_baudrate(hdev, len)) {
				nxpdev->baudrate_changed = changed;
				serdev_device_set_baudrate(nxpdev->serdev,
							   nxpdev->secondary_baudrate);
				serdev_device_set_flow_control(nxpdev->serdev, true);
				nxpdev->current_baudrate = nxpdev->secondary_baudrate;
			}
			goto free_skb;
		}
	}

	if (!nxp_data->helper_fw_name || nxpdev->helper_downloaded) {
		if (nxp_request_firmware(hdev, nxp_data->fw_name, nxp_data->fw_name_old))
			goto free_skb;
	} else if (nxp_data->helper_fw_name && !nxpdev->helper_downloaded) {
		if (nxp_request_firmware(hdev, nxp_data->helper_fw_name, NULL))
			goto free_skb;
	}

	if (!len) {
		bt_dev_info(hdev, "FW Download Complete: %zu bytes",
			   nxpdev->fw->size);
		if (nxp_data->helper_fw_name && !nxpdev->helper_downloaded) {
			nxpdev->helper_downloaded = true;
			serdev_device_wait_until_sent(nxpdev->serdev, 0);
			serdev_device_set_baudrate(nxpdev->serdev,
						   HCI_NXP_SEC_BAUDRATE_3M);
			serdev_device_set_flow_control(nxpdev->serdev, true);
		} else {
			clear_bit(BTNXPUART_FW_DOWNLOADING, &nxpdev->tx_state);
			wake_up_interruptible(&nxpdev->fw_dnld_done_wait_q);
		}
		goto free_skb;
	}
	if (len & 0x01) {
		/* The CRC did not match at the other end.
		 * Simply send the same bytes again.
		 */
		len = nxpdev->fw_v1_sent_bytes;
		bt_dev_dbg(hdev, "CRC error. Resend %d bytes of FW.", len);
	} else {
		nxpdev->fw_dnld_v1_offset += nxpdev->fw_v1_sent_bytes;

		/* The FW bin file is made up of many blocks of
		 * 16 byte header and payload data chunks. If the
		 * FW has requested a header, read the payload length
		 * info from the header, before sending the header.
		 * In the next iteration, the FW should request the
		 * payload data chunk, which should be equal to the
		 * payload length read from header. If there is a
		 * mismatch, clearly the driver and FW are out of sync,
		 * and we need to re-send the previous header again.
		 */
		if (len == nxpdev->fw_v1_expected_len) {
			if (len == HDR_LEN)
				nxpdev->fw_v1_expected_len = nxp_get_data_len(nxpdev->fw->data +
									nxpdev->fw_dnld_v1_offset);
			else
				nxpdev->fw_v1_expected_len = HDR_LEN;
		} else if (len == HDR_LEN) {
			/* FW download out of sync. Send previous chunk again */
			nxpdev->fw_dnld_v1_offset -= nxpdev->fw_v1_sent_bytes;
			nxpdev->fw_v1_expected_len = HDR_LEN;
		}
	}

	if (nxpdev->fw_dnld_v1_offset + len <= nxpdev->fw->size)
		serdev_device_write_buf(nxpdev->serdev, nxpdev->fw->data +
					nxpdev->fw_dnld_v1_offset, len);
	nxpdev->fw_v1_sent_bytes = len;

free_skb:
	kfree_skb(skb);
	return 0;
}

static char *nxp_get_fw_name_from_chipid(struct hci_dev *hdev, u16 chipid,
					 u8 loader_ver)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	char *fw_name = NULL;

	switch (chipid) {
	case CHIP_ID_W9098:
		fw_name = FIRMWARE_W9098;
		break;
	case CHIP_ID_IW416:
		fw_name = FIRMWARE_IW416;
		break;
	case CHIP_ID_IW612:
		fw_name = FIRMWARE_IW612;
		break;
	case CHIP_ID_IW624a:
	case CHIP_ID_IW624c:
		nxpdev->boot_reg_offset = 1;
		if ((loader_ver & FW_SECURE_MASK) == FW_OPEN)
			fw_name = FIRMWARE_IW624;
		else if ((loader_ver & FW_SECURE_MASK) != FW_AUTH_ILLEGAL)
			fw_name = FIRMWARE_SECURE_IW624;
		else
			bt_dev_err(hdev, "Illegal loader version %02x", loader_ver);
		break;
	case CHIP_ID_AW693a0:
		if ((loader_ver & FW_SECURE_MASK) == FW_OPEN)
			fw_name = FIRMWARE_AW693;
		else if ((loader_ver & FW_SECURE_MASK) != FW_AUTH_ILLEGAL)
			fw_name = FIRMWARE_SECURE_AW693;
		else
			bt_dev_err(hdev, "Illegal loader version %02x", loader_ver);
		break;
	case CHIP_ID_AW693a1:
		if ((loader_ver & FW_SECURE_MASK) == FW_OPEN)
			fw_name = FIRMWARE_AW693_A1;
		else if ((loader_ver & FW_SECURE_MASK) != FW_AUTH_ILLEGAL)
			fw_name = FIRMWARE_SECURE_AW693_A1;
		else
			bt_dev_err(hdev, "Illegal loader version %02x", loader_ver);
		break;
	case CHIP_ID_IW610a0:
	case CHIP_ID_IW610a1:
		if ((loader_ver & FW_SECURE_MASK) == FW_OPEN)
			fw_name = FIRMWARE_IW610;
		else if ((loader_ver & FW_SECURE_MASK) != FW_AUTH_ILLEGAL)
			fw_name = FIRMWARE_SECURE_IW610;
		else
			bt_dev_err(hdev, "Illegal loader version %02x", loader_ver);
		break;
	default:
		bt_dev_err(hdev, "Unknown chip signature %04x", chipid);
		break;
	}
	return fw_name;
}

static char *nxp_get_old_fw_name_from_chipid(struct hci_dev *hdev, u16 chipid,
					 u8 loader_ver)
{
	char *fw_name_old = NULL;

	switch (chipid) {
	case CHIP_ID_W9098:
		fw_name_old = FIRMWARE_W9098_OLD;
		break;
	case CHIP_ID_IW416:
		fw_name_old = FIRMWARE_IW416_OLD;
		break;
	}
	return fw_name_old;
}

static int nxp_recv_chip_ver_v3(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct v3_start_ind *req = skb_pull_data(skb, sizeof(*req));
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	const char *fw_name;
	const char *fw_name_old;
	u16 chip_id;
	u8 loader_ver;

	if (!process_boot_signature(nxpdev))
		goto free_skb;

	chip_id = le16_to_cpu(req->chip_id);
	loader_ver = req->loader_ver;
	bt_dev_info(hdev, "ChipID: %04x, Version: %d", chip_id, loader_ver);
	fw_name = nxp_get_fw_name_from_chipid(hdev, chip_id, loader_ver);
	fw_name_old = nxp_get_old_fw_name_from_chipid(hdev, chip_id, loader_ver);
	if (!nxp_request_firmware(hdev, fw_name, fw_name_old))
		nxp_send_ack(NXP_ACK_V3, hdev);

free_skb:
	kfree_skb(skb);
	return 0;
}

static void nxp_handle_fw_download_error(struct hci_dev *hdev, struct v3_data_req *req)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	__u32 offset = __le32_to_cpu(req->offset);
	__u16 err = __le16_to_cpu(req->error);
	union nxp_v3_rx_timeout_nak_u timeout_nak_buf;
	union nxp_v3_crc_nak_u crc_nak_buf;

	if (err & NXP_CRC_RX_ERROR) {
		crc_nak_buf.pkt.nak = NXP_CRC_ERROR_V3;
		crc_nak_buf.pkt.crc = crc8(crc8_table, crc_nak_buf.buf,
					   sizeof(crc_nak_buf) - 1, 0xff);
		serdev_device_write_buf(nxpdev->serdev, crc_nak_buf.buf,
					sizeof(crc_nak_buf));
	} else if (err & NXP_ACK_RX_TIMEOUT ||
		   err & NXP_HDR_RX_TIMEOUT ||
		   err & NXP_DATA_RX_TIMEOUT) {
		timeout_nak_buf.pkt.nak = NXP_NAK_V3;
		timeout_nak_buf.pkt.offset = __cpu_to_le32(offset);
		timeout_nak_buf.pkt.crc = crc8(crc8_table, timeout_nak_buf.buf,
					       sizeof(timeout_nak_buf) - 1, 0xff);
		serdev_device_write_buf(nxpdev->serdev, timeout_nak_buf.buf,
					sizeof(timeout_nak_buf));
	} else {
		bt_dev_err(hdev, "Unknown bootloader error code: %d", err);
	}
}

static u32 nxp_process_fw_metadata_tlv(struct hci_dev *hdev, char **payload)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	struct fw_metadata_tlv *tlv = (struct fw_metadata_tlv *)(*payload);
	u32 ret = sizeof(*tlv) + le32_to_cpu(tlv->len);

	/* Process only BT specific metadata TLVs */
	if (!(le16_to_cpu(tlv->flag) & FW_METADATA_FLAG_BT))
		goto align_and_return;

	switch (le16_to_cpu(tlv->id)) {
	case FW_METADATA_TLV_UUID:
		if (le32_to_cpu(tlv->len) == NXP_FW_UUID_SIZE)
			memcpy(nxpdev->crypto.fw_uuid,
				*payload + sizeof(*tlv), NXP_FW_UUID_SIZE);
		break;
	case FW_METADATA_TLV_ECDSA_KEY:
		if (le32_to_cpu(tlv->len) == NXP_FW_ECDSA_PUBKEY_SIZE)
			memcpy(nxpdev->crypto.ecdsa_public,
				*payload + sizeof(*tlv), NXP_FW_ECDSA_PUBKEY_SIZE);
		break;
	default:
		bt_dev_err(hdev, "Unknown metadata TLV ID: 0x%x", le16_to_cpu(tlv->id));
		break;
	}

align_and_return:
	/* Align the pointer to 4 byte structure alignment */
	ret = round_up(ret, 4);
	*payload += ret;

	return ret;
}

static void nxp_process_fw_meta_data(struct hci_dev *hdev, const struct firmware *fw)
{
	const char *metamagc = "metamagc";
	struct fw_metadata_hdr *hdr = NULL;
	struct fw_metadata_tail *tail;
	u32 hdr_crc = 0;
	u32 payload_crc = 0;
	char *payload;
	u32 payload_len = 0;

	/* FW metadata should contain at least header and tail */
	if (fw->size < (sizeof(*hdr) + sizeof(*tail)))
		return;

	tail = (struct fw_metadata_tail *)&fw->data[fw->size - sizeof(*tail)];

	/* If tail doesn't contain the string "metamagc", this is invalid FW metadata */
	if (memcmp(metamagc, tail->magic, strlen(metamagc)))
		return;

	hdr = (struct fw_metadata_hdr *)&fw->data[fw->size -
						  sizeof(*tail) -
						  tail->len];

	/* If metadata header isn't cmd24, this is invalid FW metadata */
	if (le32_to_cpu(hdr->cmd) != 24)
		return;

	/* If header CRC doesn't match, this is invalid FW metadata */
	hdr_crc = crc32_be(0, (u8 *)hdr, offsetof(struct fw_metadata_hdr, crc));
	if (hdr_crc != hdr->crc)
		return;

	/* If payload CRC doesn't match, this is invalid FW metadata */
	payload = (u8 *)hdr  + sizeof(*hdr);
	payload_crc = crc32_be(0, payload, hdr->len - 4);
	if (payload_crc != tail->crc)
		return;

	payload_len = hdr->len - sizeof(*tail);

	while (payload_len > sizeof(struct fw_metadata_tlv))
		payload_len -= nxp_process_fw_metadata_tlv(hdev, &payload);
}

static int nxp_recv_fw_req_v3(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	struct v3_data_req *req;
	__u16 len = 0;
	__u16 err = 0;
	__u32 offset;

	if (!process_boot_signature(nxpdev))
		goto free_skb;

	req = skb_pull_data(skb, sizeof(*req));
	if (!req || !nxpdev->fw)
		goto free_skb;

	err = __le16_to_cpu(req->error);

	if (!err) {
		nxp_send_ack(NXP_ACK_V3, hdev);
		if (nxpdev->timeout_changed == cmd_sent)
			nxpdev->timeout_changed = changed;
		if (nxpdev->baudrate_changed == cmd_sent)
			nxpdev->baudrate_changed = changed;
	} else {
		nxp_handle_fw_download_error(hdev, req);
		if (nxpdev->timeout_changed == cmd_sent &&
		    err == NXP_CRC_RX_ERROR) {
			nxpdev->fw_v3_offset_correction -= nxpdev->fw_v3_prev_sent;
			nxpdev->timeout_changed = not_changed;
		}
		if (nxpdev->baudrate_changed == cmd_sent &&
		    err == NXP_CRC_RX_ERROR) {
			nxpdev->fw_v3_offset_correction -= nxpdev->fw_v3_prev_sent;
			nxpdev->baudrate_changed = not_changed;
		}
		goto free_skb;
	}

	len = __le16_to_cpu(req->len);

	if (nxpdev->timeout_changed != changed) {
		nxp_fw_change_timeout(hdev, len);
		nxpdev->timeout_changed = cmd_sent;
		goto free_skb;
	}

	if (nxpdev->baudrate_changed != changed) {
		nxpdev->new_baudrate = nxpdev->secondary_baudrate;
		if (nxp_fw_change_baudrate(hdev, len)) {
			nxpdev->baudrate_changed = cmd_sent;
			serdev_device_set_baudrate(nxpdev->serdev,
						   nxpdev->secondary_baudrate);
			serdev_device_set_flow_control(nxpdev->serdev, true);
			nxpdev->current_baudrate = nxpdev->secondary_baudrate;
		}
		goto free_skb;
	}

	offset = __le32_to_cpu(req->offset);
	if (offset < nxpdev->fw_v3_offset_correction) {
		/* This scenario should ideally never occur. But if it ever does,
		 * FW is out of sync and needs a power cycle.
		 */
		bt_dev_err(hdev, "Something went wrong during FW download");
		bt_dev_err(hdev, "Please power cycle and try again");
		goto free_skb;
	}

	nxpdev->fw_dnld_v3_offset = offset - nxpdev->fw_v3_offset_correction;

	if (req->len == 0) {
		if (nxpdev->fw_dnld_v3_offset < nxpdev->fw->size)
			nxp_process_fw_meta_data(hdev, nxpdev->fw);
		bt_dev_info(hdev, "FW Download Complete: %u bytes.",
			   req->offset - nxpdev->fw_v3_offset_correction);
		clear_bit(BTNXPUART_FW_DOWNLOADING, &nxpdev->tx_state);
		wake_up_interruptible(&nxpdev->fw_dnld_done_wait_q);
		goto free_skb;
	}

	serdev_device_write_buf(nxpdev->serdev, nxpdev->fw->data +
				nxpdev->fw_dnld_v3_offset, len);

free_skb:
	nxpdev->fw_v3_prev_sent = len;
	kfree_skb(skb);
	return 0;
}

static int nxp_set_baudrate_cmd(struct hci_dev *hdev, void *data)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	__le32 new_baudrate = __cpu_to_le32(nxpdev->new_baudrate);
	struct ps_data *psdata = &nxpdev->psdata;
	struct sk_buff *skb;
	u8 *status;

	if (!psdata)
		return 0;

	skb = nxp_drv_send_cmd(hdev, HCI_NXP_SET_OPER_SPEED, 4,
			       (u8 *)&new_baudrate, true);
	if (IS_ERR(skb)) {
		bt_dev_err(hdev, "Setting baudrate failed (%ld)", PTR_ERR(skb));
		return PTR_ERR(skb);
	}

	status = (u8 *)skb_pull_data(skb, 1);
	if (status) {
		if (*status == 0) {
			serdev_device_set_baudrate(nxpdev->serdev, nxpdev->new_baudrate);
			nxpdev->current_baudrate = nxpdev->new_baudrate;
		}
		bt_dev_dbg(hdev, "Set baudrate response: status=%d, baudrate=%d",
			   *status, nxpdev->new_baudrate);
	}
	kfree_skb(skb);

	return 0;
}

static int nxp_check_boot_sign(struct btnxpuart_dev *nxpdev)
{
	serdev_device_set_baudrate(nxpdev->serdev, HCI_NXP_PRI_BAUDRATE);
	if (ind_reset_in_progress(nxpdev))
		serdev_device_set_flow_control(nxpdev->serdev, false);
	else
		serdev_device_set_flow_control(nxpdev->serdev, true);
	set_bit(BTNXPUART_CHECK_BOOT_SIGNATURE, &nxpdev->tx_state);

	return wait_event_interruptible_timeout(nxpdev->check_boot_sign_wait_q,
					       !test_bit(BTNXPUART_CHECK_BOOT_SIGNATURE,
							 &nxpdev->tx_state),
					       msecs_to_jiffies(1000));
}

static int nxp_set_ind_reset(struct hci_dev *hdev, void *data)
{
	static const u8 ir_hw_err[] = { HCI_EV_HARDWARE_ERROR,
					0x01, BTNXPUART_IR_HW_ERR };
	struct sk_buff *skb;

	skb = bt_skb_alloc(3, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;

	hci_skb_pkt_type(skb) = HCI_EVENT_PKT;
	skb_put_data(skb, ir_hw_err, 3);

	/* Inject Hardware Error to upper stack */
	return hci_recv_frame(hdev, skb);
}

/* Firmware dump */
static void nxp_coredump(struct hci_dev *hdev)
{
	struct sk_buff *skb;
	u8 pcmd = 2;

	skb = nxp_drv_send_cmd(hdev, HCI_NXP_TRIGGER_DUMP, 1, &pcmd, true);
	if (IS_ERR(skb))
		bt_dev_err(hdev, "Failed to trigger FW Dump. (%ld)", PTR_ERR(skb));
	else
		kfree_skb(skb);
}

static void nxp_coredump_hdr(struct hci_dev *hdev, struct sk_buff *skb)
{
	/* Nothing to be added in FW dump header */
}

static int nxp_process_fw_dump(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct hci_acl_hdr *acl_hdr = (struct hci_acl_hdr *)skb_pull_data(skb,
									  sizeof(*acl_hdr));
	struct nxp_fw_dump_hdr *fw_dump_hdr = (struct nxp_fw_dump_hdr *)skb->data;
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	__u16 seq_num = __le16_to_cpu(fw_dump_hdr->seq_num);
	__u16 buf_len = __le16_to_cpu(fw_dump_hdr->buf_len);
	int err;

	if (seq_num == 0x0001) {
		if (test_and_set_bit(BTNXPUART_FW_DUMP_IN_PROGRESS, &nxpdev->tx_state)) {
			bt_dev_err(hdev, "FW dump already in progress");
			goto free_skb;
		}
		bt_dev_warn(hdev, "==== Start FW dump ===");
		err = hci_devcd_init(hdev, NXP_FW_DUMP_SIZE);
		if (err < 0)
			goto free_skb;

		schedule_delayed_work(&hdev->dump.dump_timeout,
				      msecs_to_jiffies(20000));
	}

	err = hci_devcd_append(hdev, skb_clone(skb, GFP_ATOMIC));
	if (err < 0)
		goto free_skb;

	if (buf_len == 0) {
		bt_dev_warn(hdev, "==== FW dump complete ===");
		hci_devcd_complete(hdev);
		nxp_set_ind_reset(hdev, NULL);
	}

free_skb:
	kfree_skb(skb);
	return 0;
}

static int nxp_recv_acl_pkt(struct hci_dev *hdev, struct sk_buff *skb)
{
	__u16 handle = __le16_to_cpu(hci_acl_hdr(skb)->handle);

	/* FW dump chunks are ACL packets with conn handle 0xfff */
	if ((handle & 0x0FFF) == 0xFFF)
		return nxp_process_fw_dump(hdev, skb);
	else
		return hci_recv_frame(hdev, skb);
}

static int nxp_set_bdaddr(struct hci_dev *hdev, const bdaddr_t *bdaddr)
{
	union nxp_set_bd_addr_payload pcmd;
	int err;

	pcmd.data.param_id = 0xfe;
	pcmd.data.param_len = 6;
	memcpy(pcmd.data.param, bdaddr, 6);

	/* BD address can be assigned only after first reset command. */
	err = __hci_cmd_sync_status(hdev, HCI_OP_RESET, 0, NULL,
				    HCI_INIT_TIMEOUT);
	if (err) {
		bt_dev_err(hdev,
			   "Reset before setting local-bd-addr failed (%d)",
			   err);
		return err;
	}

	err = __hci_cmd_sync_status(hdev, HCI_NXP_SET_BD_ADDR, sizeof(pcmd),
			     pcmd.buf, HCI_CMD_TIMEOUT);
	if (err) {
		bt_dev_err(hdev, "Changing device address failed (%d)", err);
		return err;
	}

	return 0;
}

static void nxp_handle_chip_specific_features(struct hci_dev *hdev, u8 *version)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);

	if (!version || strlen(version) == 0)
		return;

	if (!strncmp(version, "aw693n-V1", strlen("aw693n-V1")))
		nxpdev->secure_interface = true;
}

static void nxp_get_fw_version(struct hci_dev *hdev)
{
	struct sk_buff *skb;
	u8 version[100] = {0};
	u8 cmd = 0;
	u8 *status;

	skb = nxp_drv_send_cmd(hdev, HCI_NXP_GET_FW_VERSION, 1, &cmd, true);
	if (IS_ERR(skb)) {
		bt_dev_err(hdev, "Failed to get firmware version (%ld)",
			   PTR_ERR(skb));
		return;
	}

	status = skb_pull_data(skb, 1);
	if (status) {
		if (*status) {
			bt_dev_err(hdev, "Error get FW version: %d", *status);
		} else if (skb->len < 10 || skb->len >= 100) {
			bt_dev_err(hdev, "Invalid FW version");
		} else {
			memcpy(version, skb->data, skb->len);
			bt_dev_info(hdev, "FW Version: %s", version);
			nxp_handle_chip_specific_features(hdev, version);
		}
	}

	kfree_skb(skb);
}

/* Secure Interface */
static int nxp_get_pub_key(struct hci_dev *hdev,
		      const struct nxp_tls_device_info *device_info,
		      u8 ecdsa_pub_key[NXP_FW_ECDSA_PUBKEY_SIZE])
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	const char *fw_name;

	if (ecdsa_pub_key[0] == 0x04)
		return 0;

	fw_name = nxp_get_fw_name_from_chipid(hdev, device_info->chip_id,
					      device_info->device_flags);
	if (nxp_request_firmware(hdev, fw_name, NULL))
		return -ENOENT;

	nxp_process_fw_meta_data(hdev, nxpdev->fw);
	release_firmware(nxpdev->fw);
	memset(nxpdev->fw_name, 0, sizeof(nxpdev->fw_name));

	if (memcmp(nxpdev->crypto.fw_uuid, device_info->uuid, 16) ||
	    nxpdev->crypto.ecdsa_public[0] != 0x04) {
		bt_dev_err(hdev,
			   "UUID check failed while trying to read ECDSA public key from FW.");
		return -EBADF;
	}

	memcpy(ecdsa_pub_key, nxpdev->crypto.ecdsa_public, 65);

	return 0;
}

static int nxp_generate_ecdh_public_key(struct crypto_kpp *tfm, u8 public_key[64])
{
	DECLARE_CRYPTO_WAIT(result);
	struct kpp_request *req;
	u8 *tmp;
	struct scatterlist dst;
	int err;

	tmp = kzalloc(64, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	req = kpp_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		err = -ENOMEM;
		goto free_tmp;
	}

	sg_init_one(&dst, tmp, 64);
	kpp_request_set_input(req, NULL, 0);
	kpp_request_set_output(req, &dst, 64);
	kpp_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				 crypto_req_done, &result);

	err = crypto_kpp_generate_public_key(req);
	err = crypto_wait_req(err, &result);
	if (err < 0)
		goto free_all;

	memcpy(public_key, tmp, 64);

free_all:
	kpp_request_free(req);
free_tmp:
	kfree(tmp);
	return err;
}

static inline void nxp_tls_hdr_init(struct nxp_tls_message_hdr *hdr, size_t len,
				    enum nxp_tls_message_id id)
{
	hdr->magic = cpu_to_le32(NXP_TLS_MAGIC);
	hdr->len = cpu_to_le16((u16)len);
	hdr->message_id = (u8)id;
	hdr->protocol_version = NXP_TLS_VERSION;
}

static struct sk_buff *nxp_host_do_hello(struct hci_dev *hdev)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	union nxp_tls_host_hello_payload tls_hello;
	struct nxp_tls_host_hello *host_hello = &tls_hello.host_hello;
	struct ecdh p = {0};
	u8 *buf = NULL;
	unsigned int buf_len;
	struct sk_buff *skb;
	int ret;

	nxp_tls_hdr_init(&host_hello->hdr, sizeof(*host_hello), NXP_TLS_HOST_HELLO);

	host_hello->sig_alg = cpu_to_le16(NXP_TLS_ECDSA_SECP256R1_SHA256);
	host_hello->key_exchange_type = cpu_to_le16(NXP_TLS_ECDHE_SECP256R1);
	host_hello->cipher_suite = cpu_to_le16(NXP_TLS_AES_128_GCM_SHA256);

	get_random_bytes(host_hello->random, sizeof(host_hello->random));

	/* Generate random private key */
	p.key_size = 32;
	p.key = kzalloc(p.key_size, GFP_KERNEL);
	if (!p.key)
		return ERR_PTR(-ENOMEM);

	get_random_bytes(p.key, p.key_size);

	buf_len = crypto_ecdh_key_len(&p);
	buf = kzalloc(buf_len, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto free_key;
	}

	ret = crypto_ecdh_encode_key(buf, buf_len, &p);
	if (ret) {
		bt_dev_err(hdev, "crypto_ecdh_encode_key() failed");
		goto free_buf;
	}

	ret = crypto_kpp_set_secret(nxpdev->crypto.kpp, buf, buf_len);
	if (ret) {
		bt_dev_err(hdev, "crypto_kpp_set_secret() failed");
		goto free_buf;
	}

	ret = nxp_generate_ecdh_public_key(nxpdev->crypto.kpp, host_hello->pubkey);
	if (ret) {
		bt_dev_err(hdev, "Failed to generate ECDH public key: %d", ret);
		goto free_buf;
	}

	ret = crypto_shash_update(nxpdev->crypto.tls_handshake_hash_desc,
				  (u8 *)host_hello, sizeof(*host_hello));
	if (ret) {
		bt_dev_err(hdev, "Failed to update handshake hash: %d", ret);
		goto free_buf;
	}

	tls_hello.msg_type = 0;

	skb = __hci_cmd_sync(hdev, HCI_NXP_SHI_ENCRYPT, sizeof(tls_hello),
			     tls_hello.buf, HCI_CMD_TIMEOUT);
	if (IS_ERR(skb)) {
		bt_dev_err(hdev, "Host Hello command failed: %ld", PTR_ERR(skb));
		ret = PTR_ERR(skb);
	}

free_buf:
	kfree(buf);
free_key:
	memset(p.key, 0, p.key_size);
	kfree(p.key);
	if (ret)
		return ERR_PTR(ret);
	else
		return skb;
}

static int nxp_crypto_shash_final(struct shash_desc *desc, u8 *out)
{
	struct shash_desc *desc_tmp = kzalloc(sizeof(struct shash_desc) +
					      crypto_shash_descsize(desc->tfm),
					      GFP_KERNEL);

	if (!desc_tmp)
		return -ENOMEM;

	crypto_shash_export(desc, desc_tmp);
	crypto_shash_final(desc, out);
	crypto_shash_import(desc, desc_tmp);
	kfree(desc_tmp);

	return 0;
}

static int nxp_compute_shared_secret(struct crypto_kpp *tfm, const u8 public_key[64], u8 secret[32])
{
	DECLARE_CRYPTO_WAIT(result);
	struct kpp_request *req;
	struct scatterlist src, dst;
	int err;

	req = kpp_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		pr_err("Failed to allocate memory for KPP request\n");
		return -ENOMEM;
	}

	sg_init_one(&src, public_key, 64);
	sg_init_one(&dst, secret, 32);
	kpp_request_set_input(req, &src, 64);
	kpp_request_set_output(req, &dst, 32);
	kpp_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				 crypto_req_done, &result);
	err = crypto_kpp_compute_shared_secret(req);
	err = crypto_wait_req(err, &result);
	if (err < 0) {
		pr_err("alg: ecdh: compute shared secret failed. err %d\n", err);
		goto free_all;
	}

free_all:
	kpp_request_free(req);
	return err;
}

static int nxp_hkdf_sha256_extract(const void *salt, size_t salt_len,
				    const void *ikm, size_t ikm_len,
				    u8 result[SHA256_DIGEST_SIZE])
{
	struct crypto_shash *tfm;
	struct shash_desc *desc;
	u8 zeroes[SHA256_DIGEST_SIZE] = {0};
	int ret = 0;

	tfm = crypto_alloc_shash("hmac(sha256)", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);

	desc = kzalloc(sizeof(*desc) + crypto_shash_descsize(tfm), GFP_KERNEL);
	if (!desc) {
		crypto_free_shash(tfm);
		return -ENOMEM;
	}

	desc->tfm = tfm;

	/* RFC 5869: If salt is empty, use HashLen zero octets */
	if (salt_len == 0)
		ret = crypto_shash_setkey(tfm, zeroes, SHA256_DIGEST_SIZE);
	else
		ret = crypto_shash_setkey(tfm, salt, salt_len);

	if (ret)
		goto cleanup;

	ret = crypto_shash_init(desc);
	if (ret)
		goto cleanup;

	ret = crypto_shash_update(desc, ikm, ikm_len);
	if (ret)
		goto cleanup;

	ret = crypto_shash_final(desc, result);

cleanup:
	kfree(desc);
	crypto_free_shash(tfm);
	return ret;
}

static int nxp_hkdf_expand_label(const u8 secret[SHA256_DIGEST_SIZE],
				 const char *label, size_t label_size,
				 u8 *context, size_t context_size,
				 void *output, size_t output_size)
{
	struct crypto_shash *tfm = crypto_alloc_shash("hmac(sha256)", 0, 0);
	struct shash_desc *desc = kzalloc(sizeof(*desc) + crypto_shash_descsize(tfm),
					  GFP_KERNEL);
	u8 hmac_out[SHA256_DIGEST_SIZE];
	u16 length = output_size;
	u8 one = 0x01;

	if (IS_ERR(tfm)) {
		pr_err("Failed to alloc shash for HMAC\n");
		return -ENOMEM;
	}

	if (!desc) {
		crypto_free_shash(tfm);
		return -ENOMEM;
	}

	crypto_shash_setkey(tfm, secret, SHA256_DIGEST_SIZE);
	desc->tfm = tfm;

	crypto_shash_init(desc);
	crypto_shash_update(desc, (u8 *)&length, sizeof(length));
	crypto_shash_update(desc, label, label_size);

	if (context && context_size > 0)
		crypto_shash_update(desc, context, context_size);

	/* RFC 5869: HKDF-Expand counter starts at 0x01 */
	crypto_shash_update(desc, &one, sizeof(one));
	crypto_shash_final(desc, hmac_out);

	memcpy(output, hmac_out, output_size);

	kfree(desc);
	crypto_free_shash(tfm);
	return 0;
}

static int nxp_hkdf_derive_secret(u8 secret[32], const char *label, size_t label_size,
				  u8 context[SHA256_DIGEST_SIZE],
				  u8 output[SHA256_DIGEST_SIZE])
{
	return nxp_hkdf_expand_label(secret, label, label_size, context, SHA256_DIGEST_SIZE,
				     output, SHA256_DIGEST_SIZE);
}

/*
 * The digital signature is computed over the concatenation of:
 *  -  A string that consists of octet 32 (0x20) repeated 64 times
 *  -  The context string
 *  -  A single 0 byte which serves as the separator
 *  -  The content to be signed
 */
static int nxp_handshake_sig_hash(const u8 transcript_hash[SHA256_DIGEST_SIZE],
				   const char *context, size_t context_len,
				   u8 output_hash[SHA256_DIGEST_SIZE])
{
	struct crypto_shash *tfm;
	struct shash_desc *desc;
	const u8 zero = 0;

	tfm = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);

	desc = kzalloc(sizeof(*desc) + crypto_shash_descsize(tfm), GFP_KERNEL);
	if (!desc) {
		crypto_free_shash(tfm);
		return -ENOMEM;
	}

	desc->tfm = tfm;

	memset(output_hash, 0x20, SHA256_DIGEST_SIZE);

	crypto_shash_init(desc);
	/* 2x hash size = block size of 0x20 */
	crypto_shash_update(desc, output_hash, SHA256_DIGEST_SIZE);
	crypto_shash_update(desc, output_hash, SHA256_DIGEST_SIZE);

	crypto_shash_update(desc, context, context_len);
	crypto_shash_update(desc, &zero, sizeof(zero));

	crypto_shash_update(desc, transcript_hash, SHA256_DIGEST_SIZE);
	crypto_shash_final(desc, output_hash);

	kfree(desc);
	crypto_free_shash(tfm);
	return 0;
}


static void nxp_aead_complete(void *req, int err)
{
	struct btnxpuart_crypto *crypto = req;

	crypto->decrypt_result = err;
	complete(&crypto->completion);
}

static int nxp_aes_gcm_decrypt(struct hci_dev *hdev, void *buf, size_t size,
			       u8 auth_tag[16], u8 key[AES_KEYSIZE_128],
			       u8 iv[GCM_AES_IV_SIZE])
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	struct crypto_aead *tfm;
	struct aead_request *req;
	struct scatterlist src, dst;
	struct nxp_tls_data_add aad = {
		.version = NXP_TLS_VERSION,
		.len = (u16)size
	};
	u8 *ciphertext;
	u8 *plaintext;
	int ret = 0;

	ciphertext = kzalloc(sizeof(aad) + size + NXP_ENC_AUTH_TAG_SIZE,
				 GFP_KERNEL);
	if (!ciphertext)
		return -ENOMEM;

	plaintext = kzalloc(size + NXP_ENC_AUTH_TAG_SIZE, GFP_KERNEL);
	if (!plaintext) {
		ret = -ENOMEM;
		goto free_ciphertext;
	}

	memcpy(ciphertext, &aad, sizeof(aad));
	memcpy(ciphertext + sizeof(aad), buf, size);
	memcpy(ciphertext + sizeof(aad) + size, auth_tag, NXP_ENC_AUTH_TAG_SIZE);

	tfm = crypto_alloc_aead("gcm(aes)", 0, 0);
	if (IS_ERR(tfm)) {
		ret = PTR_ERR(tfm);
		goto free_plaintext;
	}

	crypto_aead_setkey(tfm, key, AES_KEYSIZE_128);
	crypto_aead_setauthsize(tfm, NXP_ENC_AUTH_TAG_SIZE);

	req = aead_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		ret = -ENOMEM;
		goto free_tfm;
	}

	sg_init_one(&src, ciphertext, sizeof(aad) + size + NXP_ENC_AUTH_TAG_SIZE);
	sg_init_one(&dst, plaintext, size + NXP_ENC_AUTH_TAG_SIZE);
	init_completion(&nxpdev->crypto.completion);

	aead_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				  nxp_aead_complete, &nxpdev->crypto);
	aead_request_set_crypt(req, &src, &dst, size + NXP_ENC_AUTH_TAG_SIZE, iv);
	aead_request_set_ad(req, sizeof(aad));

	ret = crypto_aead_decrypt(req);
	if (ret == -EINPROGRESS || ret == -EBUSY) {
		wait_for_completion(&nxpdev->crypto.completion);
		ret = nxpdev->crypto.decrypt_result;
	}
	if (!ret)
		memcpy(buf, plaintext + sizeof(aad), size);

	aead_request_free(req);
free_tfm:
	crypto_free_aead(tfm);
free_plaintext:
	kfree(plaintext);
free_ciphertext:
	kfree(ciphertext);
	return ret;
}

static int nxp_aes_gcm_encrypt(struct hci_dev *hdev, void *buf, size_t size, u8 auth_tag[16],
			       u8 key[AES_KEYSIZE_128], u8 iv[GCM_AES_IV_SIZE])
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	struct crypto_aead *tfm;
	struct aead_request *req;
	struct scatterlist src, dst;
	struct nxp_tls_data_add aad = {
		.version = NXP_TLS_VERSION,
		.len = (u16)size
	};
	u8 *ciphertext;
	u8 *plaintext;
	int ret = 0;

	ciphertext = kzalloc(sizeof(aad) + size + NXP_ENC_AUTH_TAG_SIZE,
				 GFP_KERNEL);
	if (!ciphertext)
		return -ENOMEM;

	plaintext = kzalloc(size + NXP_ENC_AUTH_TAG_SIZE, GFP_KERNEL);
	if (!plaintext) {
		ret = -ENOMEM;
		goto free_ciphertext;
	}

	memcpy(plaintext, &aad, sizeof(aad));
	memcpy(plaintext + sizeof(aad), buf, size);

	tfm = crypto_alloc_aead("gcm(aes)", 0, 0);
	if (IS_ERR(tfm)) {
		ret = PTR_ERR(tfm);
		goto free_plaintext;
	}

	crypto_aead_setkey(tfm, key, AES_KEYSIZE_128);
	crypto_aead_setauthsize(tfm, NXP_ENC_AUTH_TAG_SIZE);

	req = aead_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		ret = -ENOMEM;
		goto free_tfm;
	}

	sg_init_one(&src, plaintext, size + NXP_ENC_AUTH_TAG_SIZE);
	sg_init_one(&dst, ciphertext, sizeof(aad) + size + NXP_ENC_AUTH_TAG_SIZE);
	init_completion(&nxpdev->crypto.completion);

	aead_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				  nxp_aead_complete, &nxpdev->crypto);
	aead_request_set_crypt(req, &src, &dst, size, iv);
	aead_request_set_ad(req, sizeof(aad));

	ret = crypto_aead_encrypt(req);
	if (ret == -EINPROGRESS || ret == -EBUSY) {
		wait_for_completion(&nxpdev->crypto.completion);
		ret = nxpdev->crypto.decrypt_result;
	}
	if (!ret) {
		memcpy(buf, ciphertext + sizeof(aad), size);
		memcpy(auth_tag, ciphertext + size + sizeof(aad), NXP_ENC_AUTH_TAG_SIZE);
	}

	aead_request_free(req);
free_tfm:
	crypto_free_aead(tfm);
free_plaintext:
	kfree(plaintext);
free_ciphertext:
	kfree(ciphertext);
	return ret;
}

static int nxp_handshake_decrypt_verify(struct hci_dev *hdev, void *buf, size_t size,
					u8 auth_tag[16],
					u8 traffic_secret[SHA256_DIGEST_SIZE])
{
	u8 key[AES_KEYSIZE_128] = {0};
	u8 iv[GCM_AES_IV_SIZE] = {0};

	nxp_hkdf_expand_label(traffic_secret, NXP_TLS_KEYING_KEY_LABEL, NULL, 0,
			      key, AES_KEYSIZE_128);
	nxp_hkdf_expand_label(traffic_secret, NXP_TLS_KEYING_IV_LABEL, NULL, 0,
			      iv, GCM_AES_IV_SIZE);

	return nxp_aes_gcm_decrypt(hdev, buf, size, auth_tag, key, iv);
}

static int nxp_handshake_encrypt(struct hci_dev *hdev, void *buf,
				 size_t size, u8 auth_tag[16],
				 u8 traffic_secret[SHA256_DIGEST_SIZE])
{
	u8 key[AES_KEYSIZE_128] = {0};
	u8 iv[GCM_AES_IV_SIZE] = {0};

	nxp_hkdf_expand_label(traffic_secret, NXP_TLS_KEYING_KEY_LABEL, NULL,
			      0, key, AES_KEYSIZE_128);
	nxp_hkdf_expand_label(traffic_secret, NXP_TLS_KEYING_IV_LABEL, NULL,
			      0, iv, GCM_AES_IV_SIZE);

	return nxp_aes_gcm_encrypt(hdev, buf, size, auth_tag, key, iv);
}

static int nxp_p256_ecdsa_verify(const u8 sig[64], const u8 pub[65],
				const u8 *hash, size_t hash_len)
{
	struct public_key_signature sig_info = {0};
	struct public_key pub_key = {0};
	int ret;

	sig_info.s = (u8 *)sig;
	sig_info.s_size = 64;
	sig_info.digest = (u8 *)hash;
	sig_info.digest_size = hash_len;
	sig_info.pkey_algo = "ecdsa";
	sig_info.hash_algo = "sha256";
	sig_info.encoding = "p1363";

	pub_key.key = (void *)pub;
	pub_key.keylen = 65;
	pub_key.algo = OID_id_ecPublicKey;
	pub_key.key_is_private = false;
	pub_key.pkey_algo = "ecdsa-nist-p256";
	pub_key.id_type = NULL;

	ret = public_key_verify_signature(&pub_key, &sig_info);
	if (ret)
		pr_err("ECDSA signature verification failed: %d\n", ret);

	return ret;
}

static int nxp_device_hello_sig_verify(struct hci_dev *hdev, struct nxp_tls_device_hello *msg)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	u8 hash_sig[SHA256_DIGEST_SIZE];

	nxp_handshake_sig_hash(nxpdev->crypto.handshake_h2_hash,
			       "D HS SIG", 8, hash_sig);
	return nxp_p256_ecdsa_verify(msg->enc.device_handshake_sig.sig,
				nxpdev->crypto.ecdsa_public,
				hash_sig, SHA256_DIGEST_SIZE);
}

static int nxp_write_finished(struct hci_dev *hdev,
			       const u8 hs_traffic_secret[SHA256_DIGEST_SIZE],
			       u8 verify_data[SHA256_DIGEST_SIZE])
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	u8 transcript_hash[SHA256_DIGEST_SIZE];
	u8 finished_key[SHA256_DIGEST_SIZE];
	int ret = 0;

	ret = nxp_crypto_shash_final(nxpdev->crypto.tls_handshake_hash_desc,
				     transcript_hash);
	if (ret)
		return ret;

	ret = nxp_hkdf_expand_label(hs_traffic_secret, NXP_TLS_FINISHED_LABEL,
				    NULL, 0, finished_key, sizeof(finished_key));
	if (ret)
		return ret;

	nxp_hkdf_sha256_extract(finished_key, SHA256_DIGEST_SIZE, transcript_hash,
				SHA256_DIGEST_SIZE, verify_data);

	return 0;
}

static int nxp_verify_device_finished(struct hci_dev *hdev,
				      struct nxp_tls_device_hello *msg,
				      const u8 hs_traffic_secret[SHA256_DIGEST_SIZE])
{
	u8 verify_data[SHA256_DIGEST_SIZE] = {0};
	int ret = 0;

	ret = nxp_write_finished(hdev, hs_traffic_secret, verify_data);
	if (ret)
		return ret;

	if (memcmp(verify_data, msg->enc.device_finished.verify_data,
		      SHA256_DIGEST_SIZE))
		return -EBADMSG;

	return 0;
}

static int nxp_process_device_hello(struct hci_dev *hdev, struct nxp_tls_device_hello *msg)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	struct nxp_tls_message_hdr *hdr;
	u8 hs_traffic_secret[SHA256_DIGEST_SIZE];
	u8 *shared_secret = NULL;
	int ret;

	if (!msg)
		return -EINVAL;

	hdr = &msg->hdr;

	if (le32_to_cpu(hdr->magic) != NXP_TLS_MAGIC ||
	    le16_to_cpu(hdr->len) != sizeof(*msg) ||
	    hdr->message_id != NXP_TLS_DEVICE_HELLO ||
	    hdr->protocol_version != NXP_TLS_VERSION) {
		bt_dev_err(hdev, "Invalid device hello header");
		return -EINVAL;
	}

	shared_secret = kzalloc(32, GFP_KERNEL);
	if (!shared_secret)
		return -ENOMEM;

	ret = crypto_shash_update(nxpdev->crypto.tls_handshake_hash_desc, (u8 *)msg,
			    DEVICE_HELLO_SIG_CUTOFF_POS);
	if (ret)
		goto fail;

	ret = nxp_crypto_shash_final(nxpdev->crypto.tls_handshake_hash_desc,
				     nxpdev->crypto.handshake_h2_hash);
	if (ret)
		goto fail;

	memcpy(nxpdev->crypto.ecdh_public, msg->pubkey, NXP_FW_ECDH_PUBKEY_SIZE);

	ret = nxp_compute_shared_secret(nxpdev->crypto.kpp, nxpdev->crypto.ecdh_public,
				  shared_secret);
	if (ret)
		goto fail;

	ret = nxp_hkdf_sha256_extract(NULL, 0, shared_secret, 32,
				      nxpdev->crypto.handshake_secret);
	if (ret)
		goto fail;

	ret = nxp_hkdf_derive_secret(nxpdev->crypto.handshake_secret,
				     NXP_TLS_DEVICE_HS_TS_LABEL,
				     nxpdev->crypto.handshake_h2_hash,
				     hs_traffic_secret);
	if (ret)
		goto fail;

	ret = nxp_handshake_decrypt_verify(hdev, &msg->enc, sizeof(msg->enc),
					   msg->auth_tag, hs_traffic_secret);
	if (ret)
		goto fail;

	/*
	 * Verify ECDSA signature handshake_sig using Device's public key from FW metadata.
	 *
	 * This is the key point where Device authentication happens:
	 * - Host generates a random (HostHello.random)
	 * - Device signs the entire handshake (incl. Host's random) with its
	 *   private key (DeviceHello.device_handshake_sig)
	 * - Host now verifies ECDSA signature generated by device using Device's
	 *   public key
	 *
	 * Only the device that possesses the proper private key could sign the
	 * Host's random.
	 * If the device is an impostor and does not pose a valid private key,
	 * the handshake will fail at this point.
	 */
	ret = nxp_get_pub_key(hdev, &msg->enc.device_info, nxpdev->crypto.ecdsa_public);
	if (ret)
		goto fail;

	ret = nxp_device_hello_sig_verify(hdev, msg);
	if (ret)
		goto fail;

	ret = crypto_shash_update(nxpdev->crypto.tls_handshake_hash_desc,
				  (u8 *)&msg->enc,
				  DEVICE_HELLO_FINISHED_ENC_CUTOFF_POS);
	if (ret)
		goto fail;

	ret = nxp_verify_device_finished(hdev, msg, hs_traffic_secret);
	if (ret)
		goto fail;

	ret = crypto_shash_update(nxpdev->crypto.tls_handshake_hash_desc,
				  (u8 *)&msg->enc.device_finished,
				  sizeof(msg->enc.device_finished));
	if (ret)
		goto fail;

	memset(hs_traffic_secret, 0, SHA256_DIGEST_SIZE);

fail:
	memset(shared_secret, 0, 32);
	kfree(shared_secret);
	return ret;
}

static int nxp_host_do_finished(struct hci_dev *hdev)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	union nxp_tls_host_finished_payload finished;
	struct nxp_tls_host_finished *msg = &finished.host_finished;
	u8 hs_traffic_secret[SHA256_DIGEST_SIZE];
	struct sk_buff *skb;
	u8 *status;
	int ret = 0;

	memset(msg, 0, sizeof(*msg));
	nxp_tls_hdr_init(&msg->hdr, sizeof(*msg), NXP_TLS_HOST_FINISHED);

	crypto_shash_update(nxpdev->crypto.tls_handshake_hash_desc,
			    (u8 *)msg, HOST_FINISHED_CUTOFF_POS);

	ret = nxp_hkdf_derive_secret(nxpdev->crypto.handshake_secret,
				     NXP_TLS_HOST_HS_TS_LABEL,
				     nxpdev->crypto.handshake_h2_hash,
				     hs_traffic_secret);
	if (ret)
		return ret;

	ret = nxp_write_finished(hdev, hs_traffic_secret,
				 msg->enc.host_finished.verify_data);
	if (ret)
		return ret;

	crypto_shash_update(nxpdev->crypto.tls_handshake_hash_desc,
			    (u8 *)&msg->enc.host_finished, sizeof(msg->enc.host_finished));

	nxp_handshake_encrypt(hdev, &msg->enc, sizeof(msg->enc),
			      msg->auth_tag, hs_traffic_secret);

	finished.msg_type = 0x01;

	skb = __hci_cmd_sync(hdev, HCI_NXP_SHI_ENCRYPT,
			     sizeof(finished), finished.buf,
			     HCI_CMD_TIMEOUT);
	if (IS_ERR(skb)) {
		bt_dev_err(hdev, "Host Finished error %ld", PTR_ERR(skb));
		return PTR_ERR(skb);
	}
	status = skb_pull_data(skb, 1);
	if (!status) {
		ret = -EIO;
		goto fail;
	}
	if (*status) {
		ret = -EIO;
		bt_dev_err(hdev, "Host Finished status error: %d", *status);
	}

fail:
	kfree_skb(skb);
	return ret;
}

static void nxp_handshake_derive_master_secret(u8 master_secret[SHA256_DIGEST_SIZE],
					       u8 handshake_secret[SHA256_DIGEST_SIZE])
{
	u8 zeros[SHA256_DIGEST_SIZE] = {0};
	u8 dhs[SHA256_DIGEST_SIZE];

	/* Derive intermediate secret */
	nxp_hkdf_expand_label(handshake_secret, NXP_TLS_DERIVED_LABEL,
			      NULL, 0, dhs, sizeof(dhs));
	/* Extract master secret from derived handshake secret */
	nxp_hkdf_sha256_extract(dhs, SHA256_DIGEST_SIZE, zeros,
				sizeof(zeros), master_secret);

	memset(dhs, 0, sizeof(dhs));
}

static int nxp_handshake_derive_traffic_keys(struct hci_dev *hdev)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	struct nxp_tls_traffic_keys *keys = &nxpdev->crypto.keys;
	u8 hash[SHA256_DIGEST_SIZE];
	int ret = 0;

	ret = crypto_shash_final(nxpdev->crypto.tls_handshake_hash_desc, hash);
	if (ret)
		return ret;

	ret = nxp_hkdf_derive_secret(nxpdev->crypto.master_secret,
				     NXP_TLS_D_AP_TS_LABEL, hash, keys->d2h_secret);
	if (ret)
		return ret;

	ret = nxp_hkdf_expand_label(keys->d2h_secret,
				    NXP_TLS_KEYING_KEY_LABEL, NULL, 0,
				    keys->d2h_key, AES_KEYSIZE_128);
	if (ret)
		return ret;

	ret = nxp_hkdf_expand_label(keys->d2h_secret,
				    NXP_TLS_KEYING_IV_LABEL, NULL, 0,
				    keys->d2h_iv, GCM_AES_IV_SIZE);
	if (ret)
		return ret;

	ret = nxp_hkdf_derive_secret(nxpdev->crypto.master_secret,
				     NXP_TLS_H_AP_TS_LABEL, hash, keys->h2d_secret);
	if (ret)
		return ret;

	ret = nxp_hkdf_expand_label(keys->h2d_secret,
				    NXP_TLS_KEYING_KEY_LABEL, NULL, 0,
				    keys->h2d_key, AES_KEYSIZE_128);
	if (ret)
		return ret;

	ret = nxp_hkdf_expand_label(keys->h2d_secret,
				    NXP_TLS_KEYING_IV_LABEL, NULL, 0,
				    keys->h2d_iv, GCM_AES_IV_SIZE);
	if (ret)
		return ret;

	memset(hash, 0, sizeof(hash));
	return ret;
}

static int nxp_authenticate_device(struct hci_dev *hdev)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	struct nxp_tls_device_hello *device_hello;
	size_t desc_size = 0;
	struct sk_buff *skb;
	u8 *status;
	int ret = 0;

	nxpdev->crypto.tls_handshake_hash_tfm = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(nxpdev->crypto.tls_handshake_hash_tfm))
		return PTR_ERR(nxpdev->crypto.tls_handshake_hash_tfm);

	desc_size = sizeof(struct shash_desc) +
		    crypto_shash_descsize(nxpdev->crypto.tls_handshake_hash_tfm);
	nxpdev->crypto.tls_handshake_hash_desc = kzalloc(desc_size, GFP_KERNEL);
	if (!nxpdev->crypto.tls_handshake_hash_desc) {
		ret = -ENOMEM;
		goto free_tfm;
	}

	nxpdev->crypto.kpp = crypto_alloc_kpp("ecdh-nist-p256", 0, 0);
	if (IS_ERR(nxpdev->crypto.kpp)) {
		ret = PTR_ERR(nxpdev->crypto.kpp);
		goto free_desc;
	}

	nxpdev->crypto.tls_handshake_hash_desc->tfm = nxpdev->crypto.tls_handshake_hash_tfm;
	crypto_shash_init(nxpdev->crypto.tls_handshake_hash_desc);

	skb = nxp_host_do_hello(hdev);
	if (IS_ERR(skb)) {
		ret =  PTR_ERR(skb);
		goto free_kpp;
	}

	status = skb_pull_data(skb, 1);
	if (*status)
		goto free_skb;

	if (skb->len != sizeof(struct nxp_tls_device_hello)) {
		bt_dev_err(hdev, "Invalid Device Hello Length: %d", skb->len);
		goto free_skb;
	}

	device_hello = skb_pull_data(skb, sizeof(*device_hello));
	ret = nxp_process_device_hello(hdev, device_hello);
	if (ret)
		goto free_skb;

	ret = nxp_host_do_finished(hdev);
	if (ret)
		goto free_skb;

	nxp_handshake_derive_master_secret(nxpdev->crypto.master_secret,
					   nxpdev->crypto.handshake_secret);

	nxp_handshake_derive_traffic_keys(hdev);

free_skb:
	kfree_skb(skb);
free_kpp:
	crypto_free_kpp(nxpdev->crypto.kpp);
	nxpdev->crypto.kpp = NULL;
free_desc:
	kfree(nxpdev->crypto.tls_handshake_hash_desc);
	nxpdev->crypto.tls_handshake_hash_desc = NULL;
free_tfm:
	crypto_free_shash(nxpdev->crypto.tls_handshake_hash_tfm);
	nxpdev->crypto.tls_handshake_hash_tfm = NULL;
	if (ret)
		bt_dev_err(hdev, "Device Authentication failed: %d", ret);

	return ret;
}

/* NXP protocol */
static int nxp_setup(struct hci_dev *hdev)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	struct serdev_device *serdev = nxpdev->serdev;
	char device_string[30];
	char event_string[50];
	char *envp[] = {device_string, event_string, NULL};
	int err = 0;

	if (nxp_check_boot_sign(nxpdev)) {
		bt_dev_dbg(hdev, "Need FW Download.");
		err = nxp_download_firmware(hdev);
		if (err < 0)
			return err;
	} else {
		bt_dev_info(hdev, "FW already running.");
		clear_bit(BTNXPUART_FW_DOWNLOADING, &nxpdev->tx_state);
	}

	snprintf(device_string, 30, "BTNXPUART_DEV=%s", dev_name(&serdev->dev));
	snprintf(event_string, 50, "BTNXPUART_STATE=FW_READY");
	bt_dev_dbg(hdev, "==== Send uevent: %s:%s ===", device_string,
		   event_string);
	kobject_uevent_env(&serdev->dev.kobj, KOBJ_CHANGE, envp);

	serdev_device_set_baudrate(nxpdev->serdev, nxpdev->fw_init_baudrate);
	nxpdev->current_baudrate = nxpdev->fw_init_baudrate;

	nxp_get_fw_version(hdev);

	if (nxpdev->secure_interface) {
		err = nxp_authenticate_device(hdev);
		if (err)
			return -EACCES;
	}

	ps_init(hdev);

	if (test_and_clear_bit(BTNXPUART_IR_IN_PROGRESS, &nxpdev->tx_state))
		hci_dev_clear_flag(hdev, HCI_SETUP);

	return 0;
}

static int nxp_post_init(struct hci_dev *hdev)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	struct ps_data *psdata = &nxpdev->psdata;

	if (nxpdev->current_baudrate != nxpdev->secondary_baudrate) {
		nxpdev->new_baudrate = nxpdev->secondary_baudrate;
		nxp_set_baudrate_cmd(hdev, NULL);
	}
	if (psdata->cur_h2c_wakeupmode != psdata->h2c_wakeupmode)
		send_wakeup_method_cmd(hdev, NULL);
	if (psdata->cur_psmode != psdata->target_ps_mode)
		send_ps_cmd(hdev, NULL);
	return 0;
}

static void nxp_hw_err(struct hci_dev *hdev, u8 code)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);

	switch (code) {
	case BTNXPUART_IR_HW_ERR:
		set_bit(BTNXPUART_IR_IN_PROGRESS, &nxpdev->tx_state);
		hci_dev_set_flag(hdev, HCI_SETUP);
		break;
	default:
		break;
	}
}

static int nxp_shutdown(struct hci_dev *hdev)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	struct sk_buff *skb;
	u8 pcmd = 0;

	if (ind_reset_in_progress(nxpdev)) {
		if (test_and_clear_bit(BTNXPUART_FW_DUMP_IN_PROGRESS,
				       &nxpdev->tx_state))
			skb = nxp_drv_send_cmd(hdev, HCI_NXP_IND_RESET, 1,
					       &pcmd, false);
		else
			skb = nxp_drv_send_cmd(hdev, HCI_NXP_IND_RESET, 1,
					       &pcmd, true);
		serdev_device_set_flow_control(nxpdev->serdev, false);
		set_bit(BTNXPUART_FW_DOWNLOADING, &nxpdev->tx_state);
		/* HCI_NXP_IND_RESET command may not returns any response */
		if (!IS_ERR(skb))
			kfree_skb(skb);
	}

	return 0;
}

static bool nxp_wakeup(struct hci_dev *hdev)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	struct ps_data *psdata = &nxpdev->psdata;

	if (psdata->c2h_wakeupmode != BT_HOST_WAKEUP_METHOD_NONE)
		return true;

	return false;
}

static void nxp_reset(struct hci_dev *hdev)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);

	if (!ind_reset_in_progress(nxpdev) && !fw_dump_in_progress(nxpdev)) {
		bt_dev_dbg(hdev, "CMD Timeout detected. Resetting.");
		nxp_set_ind_reset(hdev, NULL);
	}
}

static int btnxpuart_queue_skb(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);

	/* Prepend skb with frame type */
	memcpy(skb_push(skb, 1), &hci_skb_pkt_type(skb), 1);
	skb_queue_tail(&nxpdev->txq, skb);
	btnxpuart_tx_wakeup(nxpdev);
	return 0;
}

static int nxp_enqueue(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	struct ps_data *psdata = &nxpdev->psdata;
	struct hci_command_hdr *hdr;
	struct psmode_cmd_payload ps_parm;
	struct wakeup_cmd_payload wakeup_parm;
	__le32 baudrate_parm;

	if (fw_dump_in_progress(nxpdev))
		return -EBUSY;

	/* if vendor commands are received from user space (e.g. hcitool), update
	 * driver flags accordingly and ask driver to re-send the command to FW.
	 * In case the payload for any command does not match expected payload
	 * length, let the firmware and user space program handle it, or throw
	 * an error.
	 */
	if (bt_cb(skb)->pkt_type == HCI_COMMAND_PKT && !psdata->driver_sent_cmd) {
		hdr = (struct hci_command_hdr *)skb->data;
		if (hdr->plen != (skb->len - HCI_COMMAND_HDR_SIZE))
			return btnxpuart_queue_skb(hdev, skb);

		switch (__le16_to_cpu(hdr->opcode)) {
		case HCI_NXP_AUTO_SLEEP_MODE:
			if (hdr->plen == sizeof(ps_parm)) {
				memcpy(&ps_parm, skb->data + HCI_COMMAND_HDR_SIZE, hdr->plen);
				if (ps_parm.ps_cmd == BT_PS_ENABLE)
					psdata->target_ps_mode = PS_MODE_ENABLE;
				else if (ps_parm.ps_cmd == BT_PS_DISABLE)
					psdata->target_ps_mode = PS_MODE_DISABLE;
				psdata->c2h_ps_interval = __le16_to_cpu(ps_parm.c2h_ps_interval);
				hci_cmd_sync_queue(hdev, send_ps_cmd, NULL, NULL);
				goto free_skb;
			}
			break;
		case HCI_NXP_WAKEUP_METHOD:
			if (hdr->plen == sizeof(wakeup_parm)) {
				memcpy(&wakeup_parm, skb->data + HCI_COMMAND_HDR_SIZE, hdr->plen);
				psdata->c2h_wakeupmode = wakeup_parm.c2h_wakeupmode;
				psdata->c2h_wakeup_gpio = wakeup_parm.c2h_wakeup_gpio;
				psdata->h2c_wakeup_gpio = wakeup_parm.h2c_wakeup_gpio;
				switch (wakeup_parm.h2c_wakeupmode) {
				case BT_CTRL_WAKEUP_METHOD_GPIO:
					psdata->h2c_wakeupmode = WAKEUP_METHOD_GPIO;
					break;
				case BT_CTRL_WAKEUP_METHOD_DSR:
					psdata->h2c_wakeupmode = WAKEUP_METHOD_DTR;
					break;
				case BT_CTRL_WAKEUP_METHOD_BREAK:
				default:
					psdata->h2c_wakeupmode = WAKEUP_METHOD_BREAK;
					break;
				}
				hci_cmd_sync_queue(hdev, send_wakeup_method_cmd, NULL, NULL);
				goto free_skb;
			}
			break;
		case HCI_NXP_SET_OPER_SPEED:
			if (hdr->plen == sizeof(baudrate_parm)) {
				memcpy(&baudrate_parm, skb->data + HCI_COMMAND_HDR_SIZE, hdr->plen);
				nxpdev->new_baudrate = __le32_to_cpu(baudrate_parm);
				hci_cmd_sync_queue(hdev, nxp_set_baudrate_cmd, NULL, NULL);
				goto free_skb;
			}
			break;
		case HCI_NXP_IND_RESET:
			if (hdr->plen == 1) {
				hci_cmd_sync_queue(hdev, nxp_set_ind_reset, NULL, NULL);
				goto free_skb;
			}
			break;
		default:
			break;
		}
	}

	return btnxpuart_queue_skb(hdev, skb);

free_skb:
	kfree_skb(skb);
	return 0;
}

static struct sk_buff *nxp_dequeue(void *data)
{
	struct btnxpuart_dev *nxpdev = (struct btnxpuart_dev *)data;

	ps_start_timer(nxpdev);
	return skb_dequeue(&nxpdev->txq);
}

/* btnxpuart based on serdev */
static void btnxpuart_tx_work(struct work_struct *work)
{
	struct btnxpuart_dev *nxpdev = container_of(work, struct btnxpuart_dev,
						   tx_work);
	struct serdev_device *serdev = nxpdev->serdev;
	struct hci_dev *hdev = nxpdev->hdev;
	struct sk_buff *skb;
	int len;

	if (ps_wakeup(nxpdev))
		return;

	while ((skb = nxp_dequeue(nxpdev))) {
		len = serdev_device_write_buf(serdev, skb->data, skb->len);
		hdev->stat.byte_tx += len;

		skb_pull(skb, len);
		if (skb->len > 0) {
			skb_queue_head(&nxpdev->txq, skb);
			continue;
		}

		switch (hci_skb_pkt_type(skb)) {
		case HCI_COMMAND_PKT:
			hdev->stat.cmd_tx++;
			break;
		case HCI_ACLDATA_PKT:
			hdev->stat.acl_tx++;
			break;
		case HCI_SCODATA_PKT:
			hdev->stat.sco_tx++;
			break;
		}

		kfree_skb(skb);
	}
	clear_bit(BTNXPUART_TX_STATE_ACTIVE, &nxpdev->tx_state);
}

static int btnxpuart_open(struct hci_dev *hdev)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	int err = 0;

	err = serdev_device_open(nxpdev->serdev);
	if (err) {
		bt_dev_err(hdev, "Unable to open UART device %s",
			   dev_name(&nxpdev->serdev->dev));
	} else {
		set_bit(BTNXPUART_SERDEV_OPEN, &nxpdev->tx_state);
	}
	return err;
}

static int btnxpuart_close(struct hci_dev *hdev)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);

	serdev_device_close(nxpdev->serdev);
	skb_queue_purge(&nxpdev->txq);
	if (!IS_ERR_OR_NULL(nxpdev->rx_skb)) {
		kfree_skb(nxpdev->rx_skb);
		nxpdev->rx_skb = NULL;
	}
	clear_bit(BTNXPUART_SERDEV_OPEN, &nxpdev->tx_state);
	return 0;
}

static int btnxpuart_flush(struct hci_dev *hdev)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);

	/* Flush any pending characters */
	serdev_device_write_flush(nxpdev->serdev);
	skb_queue_purge(&nxpdev->txq);

	cancel_work_sync(&nxpdev->tx_work);

	if (!IS_ERR_OR_NULL(nxpdev->rx_skb)) {
		kfree_skb(nxpdev->rx_skb);
		nxpdev->rx_skb = NULL;
	}

	return 0;
}

static const struct h4_recv_pkt nxp_recv_pkts[] = {
	{ H4_RECV_ACL,          .recv = nxp_recv_acl_pkt },
	{ H4_RECV_SCO,          .recv = hci_recv_frame },
	{ H4_RECV_EVENT,        .recv = hci_recv_frame },
	{ H4_RECV_ISO,		.recv = hci_recv_frame },
	{ NXP_RECV_CHIP_VER_V1, .recv = nxp_recv_chip_ver_v1 },
	{ NXP_RECV_FW_REQ_V1,   .recv = nxp_recv_fw_req_v1 },
	{ NXP_RECV_CHIP_VER_V3, .recv = nxp_recv_chip_ver_v3 },
	{ NXP_RECV_FW_REQ_V3,   .recv = nxp_recv_fw_req_v3 },
};

static size_t btnxpuart_receive_buf(struct serdev_device *serdev,
				    const u8 *data, size_t count)
{
	struct btnxpuart_dev *nxpdev = serdev_device_get_drvdata(serdev);

	ps_start_timer(nxpdev);

	nxpdev->rx_skb = h4_recv_buf(&nxpdev->hu, nxpdev->rx_skb, data, count,
				     nxp_recv_pkts, ARRAY_SIZE(nxp_recv_pkts));
	if (IS_ERR(nxpdev->rx_skb)) {
		int err = PTR_ERR(nxpdev->rx_skb);
		/* Safe to ignore out-of-sync bootloader signatures */
		if (!is_fw_downloading(nxpdev) &&
		    !ind_reset_in_progress(nxpdev))
			bt_dev_err(nxpdev->hdev, "Frame reassembly failed (%d)", err);
		return count;
	}
	if (!is_fw_downloading(nxpdev) &&
	    !ind_reset_in_progress(nxpdev))
		nxpdev->hdev->stat.byte_rx += count;
	return count;
}

static void btnxpuart_write_wakeup(struct serdev_device *serdev)
{
	serdev_device_write_wakeup(serdev);
}

static const struct serdev_device_ops btnxpuart_client_ops = {
	.receive_buf = btnxpuart_receive_buf,
	.write_wakeup = btnxpuart_write_wakeup,
};

static void nxp_coredump_notify(struct hci_dev *hdev, int state)
{
	struct btnxpuart_dev *nxpdev = hci_get_drvdata(hdev);
	struct serdev_device *serdev = nxpdev->serdev;
	char device_string[30];
	char event_string[50];
	char *envp[] = {device_string, event_string, NULL};

	snprintf(device_string, 30, "BTNXPUART_DEV=%s", dev_name(&serdev->dev));
	switch (state) {
	case HCI_DEVCOREDUMP_ACTIVE:
		snprintf(event_string, 50, "BTNXPUART_STATE=FW_DUMP_ACTIVE");
		break;
	case HCI_DEVCOREDUMP_DONE:
		snprintf(event_string, 50, "BTNXPUART_STATE=FW_DUMP_DONE");
		break;
	case HCI_DEVCOREDUMP_TIMEOUT:
		snprintf(event_string, 50, "BTNXPUART_STATE=FW_DUMP_TIMEOUT");
		break;
	default:
		snprintf(event_string, 50, "BTNXPUART_STATE=FW_DUMP_STATE_%d",
			 state);
		break;
	}
	bt_dev_dbg(hdev, "==== Send uevent: %s:%s ===", device_string,
		   event_string);
	kobject_uevent_env(&serdev->dev.kobj, KOBJ_CHANGE, envp);
}

static int nxp_serdev_probe(struct serdev_device *serdev)
{
	struct hci_dev *hdev;
	struct btnxpuart_dev *nxpdev;
	bdaddr_t ba = {0};
	int err;

	nxpdev = devm_kzalloc(&serdev->dev, sizeof(*nxpdev), GFP_KERNEL);
	if (!nxpdev)
		return -ENOMEM;

	nxpdev->nxp_data = (struct btnxpuart_data *)device_get_match_data(&serdev->dev);

	nxpdev->serdev = serdev;
	serdev_device_set_drvdata(serdev, nxpdev);

	serdev_device_set_client_ops(serdev, &btnxpuart_client_ops);

	INIT_WORK(&nxpdev->tx_work, btnxpuart_tx_work);
	skb_queue_head_init(&nxpdev->txq);

	init_waitqueue_head(&nxpdev->fw_dnld_done_wait_q);
	init_waitqueue_head(&nxpdev->check_boot_sign_wait_q);

	device_property_read_u32(&nxpdev->serdev->dev, "fw-init-baudrate",
				 &nxpdev->fw_init_baudrate);
	if (!nxpdev->fw_init_baudrate)
		nxpdev->fw_init_baudrate = FW_INIT_BAUDRATE;

	device_property_read_u32(&nxpdev->serdev->dev, "max-speed",
				 &nxpdev->secondary_baudrate);
	if (!nxpdev->secondary_baudrate ||
	    (nxpdev->secondary_baudrate != HCI_NXP_SEC_BAUDRATE_3M &&
	     nxpdev->secondary_baudrate != HCI_NXP_SEC_BAUDRATE_4M)) {
		if (nxpdev->secondary_baudrate)
			dev_err(&serdev->dev,
				"Invalid max-speed. Using default 3000000.");
		nxpdev->secondary_baudrate = HCI_NXP_SEC_BAUDRATE_3M;
	}

	set_bit(BTNXPUART_FW_DOWNLOADING, &nxpdev->tx_state);

	crc8_populate_msb(crc8_table, POLYNOMIAL8);

	nxpdev->pdn = devm_reset_control_get_optional_shared(&serdev->dev, NULL);
	if (IS_ERR(nxpdev->pdn))
		return PTR_ERR(nxpdev->pdn);

	err = devm_regulator_get_enable(&serdev->dev, "vcc");
	if (err) {
		dev_err(&serdev->dev, "Failed to enable vcc regulator\n");
		return err;
	}

	/* Initialize and register HCI device */
	hdev = hci_alloc_dev();
	if (!hdev) {
		dev_err(&serdev->dev, "Can't allocate HCI device\n");
		return -ENOMEM;
	}

	reset_control_deassert(nxpdev->pdn);

	nxpdev->hdev = hdev;
	nxpdev->hu.hdev = hdev;

	hdev->bus = HCI_UART;
	hci_set_drvdata(hdev, nxpdev);

	hdev->manufacturer = MANUFACTURER_NXP;
	hdev->open  = btnxpuart_open;
	hdev->close = btnxpuart_close;
	hdev->flush = btnxpuart_flush;
	hdev->setup = nxp_setup;
	hdev->post_init = nxp_post_init;
	hdev->send  = nxp_enqueue;
	hdev->hw_error = nxp_hw_err;
	hdev->shutdown = nxp_shutdown;
	hdev->wakeup = nxp_wakeup;
	hdev->reset = nxp_reset;
	hdev->set_bdaddr = nxp_set_bdaddr;
	SET_HCIDEV_DEV(hdev, &serdev->dev);

	device_property_read_u8_array(&nxpdev->serdev->dev,
				      "local-bd-address",
				      (u8 *)&ba, sizeof(ba));
	if (bacmp(&ba, BDADDR_ANY))
		hci_set_quirk(hdev, HCI_QUIRK_USE_BDADDR_PROPERTY);

	if (hci_register_dev(hdev) < 0) {
		dev_err(&serdev->dev, "Can't register HCI device\n");
		goto probe_fail;
	}

	if (ps_setup(hdev))
		goto probe_fail;

	hci_devcd_register(hdev, nxp_coredump, nxp_coredump_hdr,
			   nxp_coredump_notify);

	return 0;

probe_fail:
	reset_control_assert(nxpdev->pdn);
	hci_free_dev(hdev);
	return -ENODEV;
}

static void nxp_serdev_remove(struct serdev_device *serdev)
{
	struct btnxpuart_dev *nxpdev = serdev_device_get_drvdata(serdev);
	struct hci_dev *hdev = nxpdev->hdev;

	if (is_fw_downloading(nxpdev)) {
		set_bit(BTNXPUART_FW_DOWNLOAD_ABORT, &nxpdev->tx_state);
		clear_bit(BTNXPUART_FW_DOWNLOADING, &nxpdev->tx_state);
		wake_up_interruptible(&nxpdev->check_boot_sign_wait_q);
		wake_up_interruptible(&nxpdev->fw_dnld_done_wait_q);
	} else {
		/* Restore FW baudrate to fw_init_baudrate if changed.
		 * This will ensure FW baudrate is in sync with
		 * driver baudrate in case this driver is re-inserted.
		 */
		if (nxpdev->current_baudrate != nxpdev->fw_init_baudrate) {
			nxpdev->new_baudrate = nxpdev->fw_init_baudrate;
			nxp_set_baudrate_cmd(hdev, NULL);
		}
	}

	ps_cleanup(nxpdev);
	hci_unregister_dev(hdev);
	reset_control_assert(nxpdev->pdn);
	hci_free_dev(hdev);
}

#ifdef CONFIG_PM_SLEEP
static int nxp_serdev_suspend(struct device *dev)
{
	struct btnxpuart_dev *nxpdev = dev_get_drvdata(dev);
	struct ps_data *psdata = &nxpdev->psdata;

	ps_control(psdata->hdev, PS_STATE_SLEEP);

	if (psdata->wakeup_source) {
		enable_irq_wake(psdata->irq_handler);
		enable_irq(psdata->irq_handler);
	}
	return 0;
}

static int nxp_serdev_resume(struct device *dev)
{
	struct btnxpuart_dev *nxpdev = dev_get_drvdata(dev);
	struct ps_data *psdata = &nxpdev->psdata;

	if (psdata->wakeup_source) {
		disable_irq(psdata->irq_handler);
		disable_irq_wake(psdata->irq_handler);
	}

	ps_control(psdata->hdev, PS_STATE_AWAKE);
	return 0;
}
#endif

#ifdef CONFIG_DEV_COREDUMP
static void nxp_serdev_coredump(struct device *dev)
{
	struct btnxpuart_dev *nxpdev = dev_get_drvdata(dev);
	struct hci_dev  *hdev = nxpdev->hdev;

	if (hdev->dump.coredump)
		hdev->dump.coredump(hdev);
}
#endif

static struct btnxpuart_data w8987_data __maybe_unused = {
	.helper_fw_name = NULL,
	.fw_name = FIRMWARE_W8987,
	.fw_name_old = FIRMWARE_W8987_OLD,
};

static struct btnxpuart_data w8997_data __maybe_unused = {
	.helper_fw_name = FIRMWARE_HELPER,
	.fw_name = FIRMWARE_W8997,
	.fw_name_old = FIRMWARE_W8997_OLD,
};

static const struct of_device_id nxpuart_of_match_table[] __maybe_unused = {
	{ .compatible = "nxp,88w8987-bt", .data = &w8987_data },
	{ .compatible = "nxp,88w8997-bt", .data = &w8997_data },
	{ }
};
MODULE_DEVICE_TABLE(of, nxpuart_of_match_table);

static const struct dev_pm_ops nxp_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(nxp_serdev_suspend, nxp_serdev_resume)
};

static struct serdev_device_driver nxp_serdev_driver = {
	.probe = nxp_serdev_probe,
	.remove = nxp_serdev_remove,
	.driver = {
		.name = "btnxpuart",
		.of_match_table = of_match_ptr(nxpuart_of_match_table),
		.pm = &nxp_pm_ops,
#ifdef CONFIG_DEV_COREDUMP
		.coredump = nxp_serdev_coredump,
#endif
	},
};

module_serdev_device_driver(nxp_serdev_driver);

MODULE_AUTHOR("Neeraj Sanjay Kale <neeraj.sanjaykale@nxp.com>");
MODULE_DESCRIPTION("NXP Bluetooth Serial driver");
MODULE_LICENSE("GPL");
