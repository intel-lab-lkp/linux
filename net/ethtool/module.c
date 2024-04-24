// SPDX-License-Identifier: GPL-2.0-only

#include <linux/ethtool.h>
#include <linux/firmware.h>
#include <linux/sfp.h>
#include <net/devlink.h>

#include "netlink.h"
#include "common.h"
#include "bitset.h"
#include "module_fw.h"

struct module_req_info {
	struct ethnl_req_info base;
};

struct module_reply_data {
	struct ethnl_reply_data	base;
	struct ethtool_module_power_mode_params power;
};

#define MODULE_REPDATA(__reply_base) \
	container_of(__reply_base, struct module_reply_data, base)

/* MODULE_GET */

const struct nla_policy ethnl_module_get_policy[ETHTOOL_A_MODULE_HEADER + 1] = {
	[ETHTOOL_A_MODULE_HEADER] = NLA_POLICY_NESTED(ethnl_header_policy),
};

static int module_get_power_mode(struct net_device *dev,
				 struct module_reply_data *data,
				 struct netlink_ext_ack *extack)
{
	const struct ethtool_ops *ops = dev->ethtool_ops;

	if (!ops->get_module_power_mode)
		return 0;

	if (dev->module_fw_flash_in_progress) {
		NL_SET_ERR_MSG(extack,
			       "Module firmware flashing is in progress");
		return -EBUSY;
	}

	return ops->get_module_power_mode(dev, &data->power, extack);
}

static int module_prepare_data(const struct ethnl_req_info *req_base,
			       struct ethnl_reply_data *reply_base,
			       const struct genl_info *info)
{
	struct module_reply_data *data = MODULE_REPDATA(reply_base);
	struct net_device *dev = reply_base->dev;
	int ret;

	ret = ethnl_ops_begin(dev);
	if (ret < 0)
		return ret;

	ret = module_get_power_mode(dev, data, info->extack);
	if (ret < 0)
		goto out_complete;

out_complete:
	ethnl_ops_complete(dev);
	return ret;
}

static int module_reply_size(const struct ethnl_req_info *req_base,
			     const struct ethnl_reply_data *reply_base)
{
	struct module_reply_data *data = MODULE_REPDATA(reply_base);
	int len = 0;

	if (data->power.policy)
		len += nla_total_size(sizeof(u8));	/* _MODULE_POWER_MODE_POLICY */

	if (data->power.mode)
		len += nla_total_size(sizeof(u8));	/* _MODULE_POWER_MODE */

	return len;
}

static int module_fill_reply(struct sk_buff *skb,
			     const struct ethnl_req_info *req_base,
			     const struct ethnl_reply_data *reply_base)
{
	const struct module_reply_data *data = MODULE_REPDATA(reply_base);

	if (data->power.policy &&
	    nla_put_u8(skb, ETHTOOL_A_MODULE_POWER_MODE_POLICY,
		       data->power.policy))
		return -EMSGSIZE;

	if (data->power.mode &&
	    nla_put_u8(skb, ETHTOOL_A_MODULE_POWER_MODE, data->power.mode))
		return -EMSGSIZE;

	return 0;
}

/* MODULE_SET */

const struct nla_policy ethnl_module_set_policy[ETHTOOL_A_MODULE_POWER_MODE_POLICY + 1] = {
	[ETHTOOL_A_MODULE_HEADER] = NLA_POLICY_NESTED(ethnl_header_policy),
	[ETHTOOL_A_MODULE_POWER_MODE_POLICY] =
		NLA_POLICY_RANGE(NLA_U8, ETHTOOL_MODULE_POWER_MODE_POLICY_HIGH,
				 ETHTOOL_MODULE_POWER_MODE_POLICY_AUTO),
};

static int
ethnl_set_module_validate(struct ethnl_req_info *req_info,
			  struct genl_info *info)
{
	const struct ethtool_ops *ops = req_info->dev->ethtool_ops;
	struct nlattr **tb = info->attrs;

	if (!tb[ETHTOOL_A_MODULE_POWER_MODE_POLICY])
		return 0;

	if (req_info->dev->module_fw_flash_in_progress) {
		NL_SET_ERR_MSG(info->extack,
			       "Module firmware flashing is in progress");
		return -EBUSY;
	}

	if (!ops->get_module_power_mode || !ops->set_module_power_mode) {
		NL_SET_ERR_MSG_ATTR(info->extack,
				    tb[ETHTOOL_A_MODULE_POWER_MODE_POLICY],
				    "Setting power mode policy is not supported by this device");
		return -EOPNOTSUPP;
	}

	return 1;
}

static int
ethnl_set_module(struct ethnl_req_info *req_info, struct genl_info *info)
{
	struct ethtool_module_power_mode_params power = {};
	struct ethtool_module_power_mode_params power_new;
	const struct ethtool_ops *ops;
	struct net_device *dev = req_info->dev;
	struct nlattr **tb = info->attrs;
	int ret;

	ops = dev->ethtool_ops;

	power_new.policy = nla_get_u8(tb[ETHTOOL_A_MODULE_POWER_MODE_POLICY]);
	ret = ops->get_module_power_mode(dev, &power, info->extack);
	if (ret < 0)
		return ret;

	if (power_new.policy == power.policy)
		return 0;

	ret = ops->set_module_power_mode(dev, &power_new, info->extack);
	return ret < 0 ? ret : 1;
}

const struct ethnl_request_ops ethnl_module_request_ops = {
	.request_cmd		= ETHTOOL_MSG_MODULE_GET,
	.reply_cmd		= ETHTOOL_MSG_MODULE_GET_REPLY,
	.hdr_attr		= ETHTOOL_A_MODULE_HEADER,
	.req_info_size		= sizeof(struct module_req_info),
	.reply_data_size	= sizeof(struct module_reply_data),

	.prepare_data		= module_prepare_data,
	.reply_size		= module_reply_size,
	.fill_reply		= module_fill_reply,

	.set_validate		= ethnl_set_module_validate,
	.set			= ethnl_set_module,
	.set_ntf_cmd		= ETHTOOL_MSG_MODULE_NTF,
};

/* MODULE_FW_FLASH_ACT */

const struct nla_policy
ethnl_module_fw_flash_act_policy[ETHTOOL_A_MODULE_FW_FLASH_PASSWORD + 1] = {
	[ETHTOOL_A_MODULE_FW_FLASH_HEADER] =
		NLA_POLICY_NESTED(ethnl_header_policy),
	[ETHTOOL_A_MODULE_FW_FLASH_FILE_NAME] = { .type = NLA_NUL_STRING },
	[ETHTOOL_A_MODULE_FW_FLASH_PASSWORD] = { .type = NLA_U32 },
};

#define MODULE_EEPROM_PHYS_ID_PAGE	0
#define MODULE_EEPROM_PHYS_ID_I2C_ADDR	0x50

static int module_flash_fw_work_init(struct ethtool_module_fw_flash *module_fw,
				     struct net_device *dev,
				     struct netlink_ext_ack *extack)
{
	const struct ethtool_ops *ops = dev->ethtool_ops;
	struct ethtool_module_eeprom page_data = {};
	u8 phys_id;
	int err;

	/* Fetch the SFF-8024 Identifier Value. For all supported standards, it
	 * is located at I2C address 0x50, byte 0. See section 4.1 in SFF-8024,
	 * revision 4.9.
	 */
	page_data.page = MODULE_EEPROM_PHYS_ID_PAGE;
	page_data.offset = SFP_PHYS_ID;
	page_data.length = sizeof(phys_id);
	page_data.i2c_address = MODULE_EEPROM_PHYS_ID_I2C_ADDR;
	page_data.data = &phys_id;

	err = ops->get_module_eeprom_by_page(dev, &page_data, extack);
	if (err < 0)
		return err;

	switch (phys_id) {
	case SFF8024_ID_QSFP_DD:
	case SFF8024_ID_OSFP:
	case SFF8024_ID_DSFP:
	case SFF8024_ID_QSFP_PLUS_CMIS:
	case SFF8024_ID_SFP_DD_CMIS:
	case SFF8024_ID_SFP_PLUS_CMIS:
		INIT_WORK(&module_fw->work, ethtool_cmis_fw_update);
		break;
	default:
		NL_SET_ERR_MSG(extack,
			       "Module type does not support firmware flashing");
		return -EOPNOTSUPP;
	}

	return 0;
}

static int __module_flash_fw_schedule(struct net_device *dev,
				      struct netlink_ext_ack *extack)
{
	struct devlink_port *devlink_port = dev->devlink_port;
	const struct ethtool_ops *ops = dev->ethtool_ops;

	if (!ops->set_module_eeprom_by_page ||
	    !ops->get_module_eeprom_by_page) {
		NL_SET_ERR_MSG(extack,
			       "Flashing module firmware is not supported by this device");
		return -EOPNOTSUPP;
	}

	if (!ops->reset) {
		NL_SET_ERR_MSG(extack,
			       "Reset module is not supported by this device, so flashing is not permitted");
		return -EOPNOTSUPP;
	}

	if (dev->module_fw_flash_in_progress) {
		NL_SET_ERR_MSG(extack, "Module firmware flashing already in progress");
		return -EBUSY;
	}

	if (dev->flags & IFF_UP) {
		NL_SET_ERR_MSG(extack, "Netdevice is up, so flashing is not permitted");
		return -EBUSY;
	}

	if (devlink_port && devlink_port->attrs.split) {
		NL_SET_ERR_MSG(extack, "Can't perform firmware flashing on a split port");
		return -EOPNOTSUPP;
	}

	return 0;
}

static int
module_flash_fw_schedule(struct net_device *dev, const char *file_name,
			 struct ethtool_module_fw_flash_params *params,
			 struct netlink_ext_ack *extack)
{
	struct ethtool_module_fw_flash *module_fw;
	int err;

	err = __module_flash_fw_schedule(dev, extack);
	if (err < 0)
		return err;

	module_fw = kzalloc(sizeof(*module_fw), GFP_KERNEL);
	if (!module_fw)
		return -ENOMEM;

	module_fw->params = *params;
	err = request_firmware_direct(&module_fw->fw, file_name, &dev->dev);
	if (err) {
		NL_SET_ERR_MSG(extack,
			       "Failed to request module firmware image");
		goto err_request_firmware;
	}

	err = module_flash_fw_work_init(module_fw, dev, extack);
	if (err < 0) {
		NL_SET_ERR_MSG(extack,
			       "Flashing module firmware is not supported by this device");
		goto err_work_init;
	}

	dev->module_fw_flash_in_progress = true;
	netdev_hold(dev, &module_fw->dev_tracker, GFP_KERNEL);
	module_fw->dev = dev;

	schedule_work(&module_fw->work);

	return 0;

err_work_init:
	release_firmware(module_fw->fw);
err_request_firmware:
	kfree(module_fw);
	return err;
}

static int module_flash_fw(struct net_device *dev, struct nlattr **tb,
			   struct genl_info *info)
{
	struct ethtool_module_fw_flash_params params = {};
	const char *file_name;
	struct nlattr *attr;

	if (GENL_REQ_ATTR_CHECK(info, ETHTOOL_A_MODULE_FW_FLASH_FILE_NAME))
		return -EINVAL;

	file_name = nla_data(tb[ETHTOOL_A_MODULE_FW_FLASH_FILE_NAME]);

	attr = tb[ETHTOOL_A_MODULE_FW_FLASH_PASSWORD];
	if (attr) {
		params.password = cpu_to_be32(nla_get_u32(attr));
		params.password_valid = true;
	}

	return module_flash_fw_schedule(dev, file_name, &params, info->extack);
}

int ethnl_act_module_fw_flash(struct sk_buff *skb, struct genl_info *info)
{
	struct ethnl_req_info req_info = {};
	struct nlattr **tb = info->attrs;
	struct net_device *dev;
	int ret;

	ret = ethnl_parse_header_dev_get(&req_info,
					 tb[ETHTOOL_A_MODULE_FW_FLASH_HEADER],
					 genl_info_net(info), info->extack,
					 true);
	if (ret < 0)
		return ret;
	dev = req_info.dev;

	rtnl_lock();
	ret = ethnl_ops_begin(dev);
	if (ret < 0)
		goto out_rtnl;

	ret = module_flash_fw(dev, tb, info);

	ethnl_ops_complete(dev);

out_rtnl:
	rtnl_unlock();
	ethnl_parse_header_dev_put(&req_info);
	return ret;
}

/* MODULE_FW_FLASH_NTF */

static void
ethnl_module_fw_flash_ntf(struct net_device *dev,
			  enum ethtool_module_fw_flash_status status,
			  const char *status_msg, u64 done, u64 total)
{
	struct sk_buff *skb;
	void *hdr;
	int ret;

	skb = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!skb)
		return;

	hdr = ethnl_bcastmsg_put(skb, ETHTOOL_MSG_MODULE_FW_FLASH_NTF);
	if (!hdr)
		goto err_skb;

	ret = ethnl_fill_reply_header(skb, dev,
				      ETHTOOL_A_MODULE_FW_FLASH_HEADER);
	if (ret < 0)
		goto err_skb;

	if (nla_put_u32(skb, ETHTOOL_A_MODULE_FW_FLASH_STATUS, status))
		goto err_skb;

	if (status_msg &&
	    nla_put_string(skb, ETHTOOL_A_MODULE_FW_FLASH_STATUS_MSG,
			   status_msg))
		goto err_skb;

	if (nla_put_u64_64bit(skb, ETHTOOL_A_MODULE_FW_FLASH_DONE, done,
			      ETHTOOL_A_MODULE_FW_FLASH_PAD))
		goto err_skb;

	if (nla_put_u64_64bit(skb, ETHTOOL_A_MODULE_FW_FLASH_TOTAL, total,
			      ETHTOOL_A_MODULE_FW_FLASH_PAD))
		goto err_skb;

	genlmsg_end(skb, hdr);
	ethnl_multicast(skb, dev);
	return;

err_skb:
	nlmsg_free(skb);
}

void ethnl_module_fw_flash_ntf_err(struct net_device *dev,
				   char *err_msg, char *sub_err_msg)
{
	char status_msg[120];

	if (sub_err_msg)
		sprintf(status_msg, "%s, %s.", err_msg, sub_err_msg);
	else
		sprintf(status_msg, "%s.", err_msg);

	ethnl_module_fw_flash_ntf(dev, ETHTOOL_MODULE_FW_FLASH_STATUS_ERROR,
				  status_msg, 0, 0);
}

void ethnl_module_fw_flash_ntf_start(struct net_device *dev)
{
	ethnl_module_fw_flash_ntf(dev, ETHTOOL_MODULE_FW_FLASH_STATUS_STARTED,
				  NULL, 0, 0);
}

void ethnl_module_fw_flash_ntf_complete(struct net_device *dev)
{
	ethnl_module_fw_flash_ntf(dev, ETHTOOL_MODULE_FW_FLASH_STATUS_COMPLETED,
				  NULL, 0, 0);
}

void ethnl_module_fw_flash_ntf_in_progress(struct net_device *dev, u64 done,
					   u64 total)
{
	ethnl_module_fw_flash_ntf(dev,
				  ETHTOOL_MODULE_FW_FLASH_STATUS_IN_PROGRESS,
				  NULL, done, total);
}
