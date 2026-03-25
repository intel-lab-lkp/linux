// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2020 - 2025 Mucse Corporation. */

#include <linux/if_ether.h>
#include <linux/bitfield.h>
#include <linux/ethtool.h>

#include "rnpgbe.h"
#include "rnpgbe_mbx.h"
#include "rnpgbe_mbx_fw.h"

/**
 * mucse_fw_send_cmd_wait_resp - Send cmd req and wait for response
 * @hw: pointer to the HW structure
 * @req: pointer to the cmd req structure
 * @reply: pointer to the fw reply structure
 *
 * mucse_fw_send_cmd_wait_resp sends req to pf-fw mailbox and wait
 * reply from fw.
 *
 * Return: 0 on success, negative errno on failure
 **/
static int mucse_fw_send_cmd_wait_resp(struct mucse_hw *hw,
				       struct mbx_fw_cmd_req *req,
				       struct mbx_fw_cmd_reply *reply)
{
	int len = le16_to_cpu(req->datalen);
	int retry_cnt = 3;
	int err;

	mutex_lock(&hw->mbx.lock);
	err = mucse_write_and_wait_ack_mbx(hw, (u32 *)req, len);
	if (err)
		goto out;
	do {
		err = mucse_poll_and_read_mbx(hw, (u32 *)reply,
					      sizeof(*reply));
		if (err)
			goto out;
		/* mucse_write_and_wait_ack_mbx return 0 means fw has
		 * received request, wait for the expect opcode
		 * reply with 'retry_cnt' times.
		 */
	} while (--retry_cnt >= 0 && reply->opcode != req->opcode);
out:
	mutex_unlock(&hw->mbx.lock);
	if (!err && retry_cnt < 0)
		return -ETIMEDOUT;
	if (!err && reply->error_code)
		return -EIO;

	return err;
}

/**
 * mucse_mbx_get_info - Get hw info from fw
 * @hw: pointer to the HW structure
 *
 * mucse_mbx_get_info tries to get hw info from hw.
 *
 * Return: 0 on success, negative errno on failure
 **/
static int mucse_mbx_get_info(struct mucse_hw *hw)
{
	struct mbx_fw_cmd_req req = {
		.datalen = cpu_to_le16(MUCSE_MBX_REQ_HDR_LEN),
		.opcode  = cpu_to_le16(GET_HW_INFO),
	};
	struct mbx_fw_cmd_reply reply = {};
	int err;

	err = mucse_fw_send_cmd_wait_resp(hw, &req, &reply);
	if (!err)
		hw->pfvfnum = FIELD_GET(GENMASK_U16(7, 0),
					le16_to_cpu(reply.hw_info.pfnum));

	return err;
}

/**
 * mucse_mbx_sync_fw - Try to sync with fw
 * @hw: pointer to the HW structure
 *
 * mucse_mbx_sync_fw tries to sync with fw. It is only called in
 * probe. Nothing (register network) todo if failed.
 * Try more times to do sync.
 *
 * Return: 0 on success, negative errno on failure
 **/
int mucse_mbx_sync_fw(struct mucse_hw *hw)
{
	int try_cnt = 3;
	int err;

	do {
		err = mucse_mbx_get_info(hw);
	} while (err == -ETIMEDOUT && try_cnt--);

	return err;
}

/**
 * mucse_mbx_powerup - Echo fw to powerup
 * @hw: pointer to the HW structure
 * @is_powerup: true for powerup, false for powerdown
 *
 * mucse_mbx_powerup echo fw to change working frequency
 * to normal after received true, and reduce working frequency
 * if false.
 *
 * Return: 0 on success, negative errno on failure
 **/
int mucse_mbx_powerup(struct mucse_hw *hw, bool is_powerup)
{
	struct mbx_fw_cmd_req req = {
		.datalen = cpu_to_le16(sizeof(req.powerup) +
				       MUCSE_MBX_REQ_HDR_LEN),
		.opcode  = cpu_to_le16(POWER_UP),
		.powerup = {
			/* fw needs this to reply correct cmd */
			.version = cpu_to_le32(GENMASK_U32(31, 0)),
			.status  = cpu_to_le32(is_powerup ? 1 : 0),
		},
	};
	int len, err;

	len = le16_to_cpu(req.datalen);
	mutex_lock(&hw->mbx.lock);
	err = mucse_write_and_wait_ack_mbx(hw, (u32 *)&req, len);
	mutex_unlock(&hw->mbx.lock);

	return err;
}

/**
 * mucse_mbx_reset_hw - Posts a mbx req to reset hw
 * @hw: pointer to the HW structure
 *
 * mucse_mbx_reset_hw posts a mbx req to firmware to reset hw.
 * We use mucse_fw_send_cmd_wait_resp to wait hw reset ok.
 *
 * Return: 0 on success, negative errno on failure
 **/
int mucse_mbx_reset_hw(struct mucse_hw *hw)
{
	struct mbx_fw_cmd_req req = {
		.datalen = cpu_to_le16(MUCSE_MBX_REQ_HDR_LEN),
		.opcode  = cpu_to_le16(RESET_HW),
	};
	struct mbx_fw_cmd_reply reply = {};

	return mucse_fw_send_cmd_wait_resp(hw, &req, &reply);
}

/**
 * mucse_mbx_get_macaddr - Posts a mbx req to request macaddr
 * @hw: pointer to the HW structure
 * @pfvfnum: index of pf/vf num
 * @mac_addr: pointer to store mac_addr
 * @port: port index
 *
 * mucse_mbx_get_macaddr posts a mbx req to firmware to get mac_addr.
 *
 * Return: 0 on success, negative errno on failure
 **/
int mucse_mbx_get_macaddr(struct mucse_hw *hw, int pfvfnum,
			  u8 *mac_addr,
			  int port)
{
	struct mbx_fw_cmd_req req = {
		.datalen      = cpu_to_le16(sizeof(req.get_mac_addr) +
					    MUCSE_MBX_REQ_HDR_LEN),
		.opcode       = cpu_to_le16(GET_MAC_ADDRESS),
		.get_mac_addr = {
			.port_mask = cpu_to_le32(BIT(port)),
			.pfvf_num  = cpu_to_le32(pfvfnum),
		},
	};
	struct mbx_fw_cmd_reply reply = {};
	int err;

	err = mucse_fw_send_cmd_wait_resp(hw, &req, &reply);
	if (err)
		return err;

	if (le32_to_cpu(reply.mac_addr.ports) & BIT(port))
		memcpy(mac_addr, reply.mac_addr.addrs[port].mac, ETH_ALEN);
	else
		return -ENODATA;

	return 0;
}

/**
 * mucse_mbx_phyup - Echo fw let the phy up
 * @hw: pointer to the HW structure
 * @is_phyup: true for up, false for down
 *
 * mucse_mbx_phyup echo fw to change phy status
 *
 * Return: 0 on success, negative errno on failure
 **/
int mucse_mbx_phyup(struct mucse_hw *hw, bool is_phyup)
{
	struct mbx_fw_cmd_req req = {
		.datalen = cpu_to_le16(sizeof(req.phy_status) +
				       MUCSE_MBX_REQ_HDR_LEN),
		.opcode  = cpu_to_le16(SET_PHY_UP),
		.phy_status = {
			.port = cpu_to_le32(hw->port),
			.status  = cpu_to_le32(is_phyup ? 1 : 0),
		},
	};
	int len, err;

	len = le16_to_cpu(req.datalen);
	mutex_lock(&hw->mbx.lock);
	err = mucse_write_and_wait_ack_mbx(hw, (u32 *)&req, len);
	mutex_unlock(&hw->mbx.lock);

	return err;
}

/**
 * mucse_mbx_link_report - Echo fw report link change event or not
 * @hw: pointer to the HW structure
 * @is_eventup: true for report, false for no
 *
 * mucse_mbx_link_eventup echo fw to change event report state
 *
 * Return: 0 on success, negative errno on failure
 **/
int mucse_mbx_link_report(struct mucse_hw *hw, bool is_report)
{
	struct mbx_fw_cmd_req req = {
		.datalen = cpu_to_le16(sizeof(req. report_status) +
				       MUCSE_MBX_REQ_HDR_LEN),
		.opcode  = cpu_to_le16(LINK_REPORT_EN),
		.report_status = {
			.port_mask = cpu_to_le16(hw->port),
			.status  = cpu_to_le16(is_report ? 1 : 0),
		},
	};
	int len, err;

	len = le16_to_cpu(req.datalen);
	mutex_lock(&hw->mbx.lock);
	err = mucse_write_and_wait_ack_mbx(hw, (u32 *)&req, len);
	mutex_unlock(&hw->mbx.lock);

	return err;
}

/**
 * mucse_update_link_status_reg - update driver speed inf to reg
 * @hw: pointer to the HW structure
 * @req: pointer to req data
 *
 * mucse_update_link_status_reg update reg according to driver info,
 * fw will send irq if status is differ with reg
 *
 **/
static void mucse_update_link_status_reg(struct mucse_hw *hw,
					 struct mbx_fw_cmd_req *req)
{
	u32 value;

	value = mucse_hw_rd32(hw, RNPGBE_LINK_ST);
	value &= ~M_ST_MASK;
	value |= M_DEFAULT_ST;

	if (le16_to_cpu(req->link_stat.port_status)) {
		value |= BIT(0);
		switch (hw->speed) {
		case 10:
			value |= (mucse_speed_10 << 8);
			break;
		case 100:
			value |= (mucse_speed_100 << 8);
			break;
		case 1000:
			value |= (mucse_speed_1000 << 8);
			break;
		default:
			/* invalid speed do nothing */
			break;
		}

		value |= (hw->duplex << 4);
		value |= (req->link_stat.st[0].s_host.pause << 24);
	} else {
		value &= ~BIT(0);
	}

	if (req->link_stat.st[0].s_host.lldp_status)
		value |= BIT(6);
	else
		value &= ~BIT(6);

	mucse_hw_wr32(hw, RNPGBE_LINK_ST, value);
}

/**
 * mucse_mbx_fw_req_handler - Handle fw req
 * @mucse: pointer to the device private structure
 * @req: pointer to req data
 *
 * rnpgbe_mbx_fw_req_handler handler fw req, such as a link event req.
 *
 * @return: 0 on success, negative on failure
 **/
static void mucse_mbx_fw_req_handler(struct mucse_hw *hw,
				     struct mbx_fw_cmd_req *req)
{
	struct mucse *mucse = container_of(hw, struct mucse, hw);
	u32 magic = le32_to_cpu(req->link_stat.port_magic);
	unsigned long flags;

	if (le16_to_cpu(req->opcode) == LINK_CHANGE_EVT) {
		spin_lock_irqsave(&mucse->link_lock, flags);

		if (le16_to_cpu(req->link_stat.port_status))
			hw->link = true;
		else
			hw->link = false;

		if (magic == ST_VALID_MAGIC) {
			hw->speed = le16_to_cpu(req->link_stat.st[0].speed);
			hw->duplex = req->link_stat.st[0].duplex;
		} else {
			hw->speed = 0;
			hw->duplex = 0;
		}
		/* update regs to notify link info is received from fw */
		mucse_update_link_status_reg(hw, req);
		mucse->flags |= M_FLAG_NEED_LINK_UPDATE;
		spin_unlock_irqrestore(&mucse->link_lock, flags);
	}
}

/**
 * mucse_fw_irq_handler - Try to handle a req from hw
 * @hw: pointer to the HW structure
 **/
void mucse_fw_irq_handler(struct mucse_hw *hw)
{
	struct mbx_fw_cmd_req req = {};

	/* try to check and read fw req */
	if (mucse_check_and_read_mbx(hw, (u32 *)&req, sizeof(req)))
		return;

	/* handle it if is a req from fw */
	if (!(le16_to_cpu(req.flags) & FLAGS_REPLY))
		mucse_mbx_fw_req_handler(hw, &req);
}
