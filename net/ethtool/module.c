// SPDX-License-Identifier: GPL-2.0-only

#include <linux/ethtool.h>
#include <linux/firmware.h>
#include <linux/sfp.h>
#include <net/devlink.h>
#include <net/netdev_lock.h>

#include "netlink.h"
#include "common.h"
#include "bitset.h"
#include "module_fw.h"
#include "cmis.h"

struct module_req_info {
	struct ethnl_req_info base;
};

struct module_reply_data {
	struct ethnl_reply_data	base;
	struct ethtool_module_power_mode_params power;
	struct ethtool_module_loopback_params loopback;
	bool loopback_supp;
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

	if (dev->ethtool->module_fw_flash_in_progress) {
		NL_SET_ERR_MSG(extack,
			       "Module firmware flashing is in progress");
		return -EBUSY;
	}

	return ops->get_module_power_mode(dev, &data->power, extack);
}

#define CMIS_PHYS_ADMINISTRATIVE_INFO_PAGE	0
#define CMIS_PHYS_ADMINISTRATIVE_INFO_OFFSET	0

#define CMIS_PAGE13_ADVERTISEMENT_PAGE		0x01
#define CMIS_PAGE13_ADVERTISEMENT_OFFSET	0x8E
#define CMIS_PAGE13_SUPPORTED			BIT(5)

#define CMIS_MODULE_LOOPBACK_CAPS_PAGE		0x13
#define CMIS_MODULE_LOOPBACK_CAPS_OFFSET	0x80

#define CMIS_MODULE_LOOPBACK_CTRL_PAGE		0x13
#define CMIS_MODULE_LOOPBACK_CTRL_OFFSET	0xB4	/* 4 consecutive bytes */
#define CMIS_MODULE_LOOPBACK_CTRL_LEN		4

/* Mapping from Page 13h control bytes to ethtool_module_loopback_types flags.
 * Byte offset 0xB4+i maps to loopback_ctrl_flags[i].
 */
static const u32 loopback_ctrl_flags[] = {
	ETHTOOL_MODULE_LOOPBACK_TYPES_MEDIA_SIDE_OUTPUT,
	ETHTOOL_MODULE_LOOPBACK_TYPES_MEDIA_SIDE_INPUT,
	ETHTOOL_MODULE_LOOPBACK_TYPES_HOST_SIDE_OUTPUT,
	ETHTOOL_MODULE_LOOPBACK_TYPES_HOST_SIDE_INPUT,
};

static bool module_is_cmis(u8 phys_id)
{
	switch (phys_id) {
	case SFF8024_ID_QSFP_DD:
	case SFF8024_ID_OSFP:
	case SFF8024_ID_DSFP:
	case SFF8024_ID_QSFP_PLUS_CMIS:
	case SFF8024_ID_SFP_DD_CMIS:
	case SFF8024_ID_SFP_PLUS_CMIS:
		return true;
	default:
	}
	return false;
}

static bool module_has_cmis_page13(u8 supp)
{
	return !!(supp & CMIS_PAGE13_SUPPORTED);
}

/* Return <0 error, >0 loopback supported, =0 no support */
static int module_get_loopback_caps(struct net_device *dev, struct netlink_ext_ack *extack,
				    u8 *caps)
{
	const struct ethtool_ops *ops = dev->ethtool_ops;
	struct ethtool_module_eeprom page_data = {};
	u8 phys_id, page13;
	int err;

	*caps = 0;

	/* Read module physical ID to verify CMIS type */
	ethtool_cmis_page_init(&page_data, CMIS_PHYS_ADMINISTRATIVE_INFO_PAGE,
			       CMIS_PHYS_ADMINISTRATIVE_INFO_OFFSET, sizeof(phys_id));
	page_data.data = &phys_id;

	err = ops->get_module_eeprom_by_page(dev, &page_data, extack);
	if (err < 0)
		return err;

	if (!module_is_cmis(phys_id))
		return 0;

	/* Read DiagnosticPagesSupported/Page13 availability */
	ethtool_cmis_page_init(&page_data, CMIS_PAGE13_ADVERTISEMENT_PAGE,
			       CMIS_PAGE13_ADVERTISEMENT_OFFSET, sizeof(page13));
	page_data.data = &page13;

	err = ops->get_module_eeprom_by_page(dev, &page_data, extack);
	if (err < 0)
		return err;

	if (!module_has_cmis_page13(page13))
		return 0;

	/* Read loopback capabilities */
	ethtool_cmis_page_init(&page_data, CMIS_MODULE_LOOPBACK_CAPS_PAGE,
			       CMIS_MODULE_LOOPBACK_CAPS_OFFSET, sizeof(*caps));
	page_data.data = caps;

	err = ops->get_module_eeprom_by_page(dev, &page_data, extack);
	if (err < 0)
		return err;

	/* Lower 4 bits map directly to ethtool_module_loopback_types flags */
	*caps &= 0x0F;
	return 1;
}

static int module_get_loopback(struct net_device *dev, struct module_reply_data *data,
			       struct netlink_ext_ack *extack)
{
	const struct ethtool_ops *ops = dev->ethtool_ops;
	struct ethtool_module_eeprom page_data = {};
	u8 ctrl[CMIS_MODULE_LOOPBACK_CTRL_LEN];
	int err, i;
	u8 caps;

	if (!ops->get_module_eeprom_by_page)
		return 0;

	if (dev->ethtool->module_fw_flash_in_progress) {
		NL_SET_ERR_MSG(extack, "Module firmware flashing is in progress");
		return -EBUSY;
	}

	err = module_get_loopback_caps(dev, extack, &caps);
	if (err < 0)
		return err;

	data->loopback_supp = (err == 1);
	data->loopback.caps = caps;

	if (!data->loopback.caps)
		return 0;

	/* Read loopback control from Page 13h, Bytes 180-183 */
	ethtool_cmis_page_init(&page_data, CMIS_MODULE_LOOPBACK_CTRL_PAGE,
			       CMIS_MODULE_LOOPBACK_CTRL_OFFSET, sizeof(ctrl));
	page_data.data = ctrl;

	err = ops->get_module_eeprom_by_page(dev, &page_data, extack);
	if (err < 0)
		return err;

	/* Each non-zero byte means that loopback type is enabled */
	for (i = 0; i < CMIS_MODULE_LOOPBACK_CTRL_LEN; i++) {
		if (ctrl[i])
			data->loopback.enabled |= loopback_ctrl_flags[i];
	}

	return 0;
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

	ret = module_get_loopback(dev, data, info->extack);
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

	if (data->loopback_supp) {
		/* _MODULE_LOOPBACK_{CAPABILITIES,ENABLED} */
		len += nla_total_size(sizeof(u32)) * 2;
	}

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

	if (data->loopback_supp &&
	    nla_put_uint(skb, ETHTOOL_A_MODULE_LOOPBACK_CAPABILITIES,
			 data->loopback.caps))
		return -EMSGSIZE;

	if (data->loopback_supp &&
	    nla_put_uint(skb, ETHTOOL_A_MODULE_LOOPBACK_ENABLED,
			 data->loopback.enabled))
		return -EMSGSIZE;

	return 0;
}

/* MODULE_SET */

const struct nla_policy ethnl_module_set_policy[ETHTOOL_A_MODULE_LOOPBACK_ENABLED + 1] = {
	[ETHTOOL_A_MODULE_HEADER] = NLA_POLICY_NESTED(ethnl_header_policy),
	[ETHTOOL_A_MODULE_POWER_MODE_POLICY] =
		NLA_POLICY_RANGE(NLA_U8, ETHTOOL_MODULE_POWER_MODE_POLICY_HIGH,
				 ETHTOOL_MODULE_POWER_MODE_POLICY_AUTO),
	[ETHTOOL_A_MODULE_LOOPBACK_ENABLED] =
		NLA_POLICY_MASK(NLA_UINT,
				ETHTOOL_MODULE_LOOPBACK_TYPES_MEDIA_SIDE_OUTPUT |
				ETHTOOL_MODULE_LOOPBACK_TYPES_MEDIA_SIDE_INPUT |
				ETHTOOL_MODULE_LOOPBACK_TYPES_HOST_SIDE_OUTPUT |
				ETHTOOL_MODULE_LOOPBACK_TYPES_HOST_SIDE_INPUT),
};

static int
ethnl_set_module_validate(struct ethnl_req_info *req_info,
			  struct genl_info *info)
{
	const struct ethtool_ops *ops = req_info->dev->ethtool_ops;
	struct nlattr **tb = info->attrs;

	if (!tb[ETHTOOL_A_MODULE_POWER_MODE_POLICY] &&
	    !tb[ETHTOOL_A_MODULE_LOOPBACK_ENABLED])
		return 0;

	if (req_info->dev->ethtool->module_fw_flash_in_progress) {
		NL_SET_ERR_MSG(info->extack,
			       "Module firmware flashing is in progress");
		return -EBUSY;
	}

	if (tb[ETHTOOL_A_MODULE_POWER_MODE_POLICY] &&
	    (!ops->get_module_power_mode || !ops->set_module_power_mode)) {
		NL_SET_ERR_MSG_ATTR(info->extack,
				    tb[ETHTOOL_A_MODULE_POWER_MODE_POLICY],
				    "Setting power mode policy is not supported by this device");
		return -EOPNOTSUPP;
	}

	if (tb[ETHTOOL_A_MODULE_LOOPBACK_ENABLED] &&
	    (!ops->get_module_eeprom_by_page || !ops->set_module_eeprom_by_page)) {
		NL_SET_ERR_MSG_ATTR(info->extack,
				    tb[ETHTOOL_A_MODULE_LOOPBACK_ENABLED],
				    "Setting loopback is not supported by this device");
		return -EOPNOTSUPP;
	}

	if (tb[ETHTOOL_A_MODULE_LOOPBACK_ENABLED] &&
	    (req_info->dev->flags & IFF_UP)) {
		NL_SET_ERR_MSG(info->extack,
			       "Netdevice is up, so setting loopback is not permitted");
		return -EBUSY;
	}

	return 1;
}

static int module_set_loopback(struct net_device *dev, struct genl_info *info)
{
	const struct ethtool_ops *ops = dev->ethtool_ops;
	struct ethtool_module_eeprom page_data = {};
	u8 ctrl[CMIS_MODULE_LOOPBACK_CTRL_LEN], caps;
	bool changed = false;
	int err, i;
	u32 req;

	req = nla_get_uint(info->attrs[ETHTOOL_A_MODULE_LOOPBACK_ENABLED]);

	err = module_get_loopback_caps(dev, info->extack, &caps);
	if (err < 0)
		return err;

	if (err == 0 || req & ~(u32)caps) {
		NL_SET_ERR_MSG(info->extack, "Requested loopback mode(s) not supported by module");
		return -EOPNOTSUPP;
	}

	/* Read current enabled state from Page 13h */
	ethtool_cmis_page_init(&page_data, CMIS_MODULE_LOOPBACK_CTRL_PAGE,
			       CMIS_MODULE_LOOPBACK_CTRL_OFFSET, sizeof(ctrl));
	page_data.data = ctrl;

	err = ops->get_module_eeprom_by_page(dev, &page_data, info->extack);
	if (err < 0)
		return err;

	/* Update control bytes: 0xFF for all-lanes enable, 0x00 for disable */
	for (i = 0; i < CMIS_MODULE_LOOPBACK_CTRL_LEN; i++) {
		u8 new_val = (req & loopback_ctrl_flags[i]) ? 0xFF : 0x00;

		if (ctrl[i] != new_val) {
			ctrl[i] = new_val;
			changed = true;
		}
	}

	if (!changed)
		return 0;

	/* Write updated control bytes */
	ethtool_cmis_page_init(&page_data, CMIS_MODULE_LOOPBACK_CTRL_PAGE,
			       CMIS_MODULE_LOOPBACK_CTRL_OFFSET, sizeof(ctrl));
	page_data.data = ctrl;

	err = ops->set_module_eeprom_by_page(dev, &page_data, info->extack);
	if (err < 0)
		return err;

	return 1;
}

static int ethnl_set_module(struct ethnl_req_info *req_info, struct genl_info *info)
{
	struct ethtool_module_power_mode_params power = {};
	struct ethtool_module_power_mode_params power_new;
	const struct ethtool_ops *ops;
	struct net_device *dev = req_info->dev;
	struct nlattr **tb = info->attrs;
	int ret = 0;

	ops = dev->ethtool_ops;

	if (tb[ETHTOOL_A_MODULE_POWER_MODE_POLICY]) {
		power_new.policy = nla_get_u8(tb[ETHTOOL_A_MODULE_POWER_MODE_POLICY]);
		ret = ops->get_module_power_mode(dev, &power, info->extack);
		if (ret < 0)
			return ret;

		if (power_new.policy != power.policy) {
			ret = ops->set_module_power_mode(dev, &power_new,
							 info->extack);
			if (ret < 0)
				return ret;
			ret = 1;
		}
	}

	if (tb[ETHTOOL_A_MODULE_LOOPBACK_ENABLED]) {
		int lret;

		lret = module_set_loopback(dev, info);
		if (lret < 0)
			return lret;
		if (lret > 0)
			ret = lret;
	}

	return ret;
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

static LIST_HEAD(module_fw_flash_work_list);
static DEFINE_SPINLOCK(module_fw_flash_work_list_lock);

static int
module_flash_fw_work_list_add(struct ethtool_module_fw_flash *module_fw,
			      struct genl_info *info)
{
	struct ethtool_module_fw_flash *work;

	/* First, check if already registered. */
	spin_lock(&module_fw_flash_work_list_lock);
	list_for_each_entry(work, &module_fw_flash_work_list, list) {
		if (work->fw_update.ntf_params.portid == info->snd_portid &&
		    work->fw_update.dev == module_fw->fw_update.dev) {
			spin_unlock(&module_fw_flash_work_list_lock);
			return -EALREADY;
		}
	}

	list_add_tail(&module_fw->list, &module_fw_flash_work_list);
	spin_unlock(&module_fw_flash_work_list_lock);

	return 0;
}

static void module_flash_fw_work_list_del(struct list_head *list)
{
	spin_lock(&module_fw_flash_work_list_lock);
	list_del(list);
	spin_unlock(&module_fw_flash_work_list_lock);
}

static void module_flash_fw_work(struct work_struct *work)
{
	struct ethtool_module_fw_flash *module_fw;

	module_fw = container_of(work, struct ethtool_module_fw_flash, work);

	ethtool_cmis_fw_update(&module_fw->fw_update);

	module_flash_fw_work_list_del(&module_fw->list);
	module_fw->fw_update.dev->ethtool->module_fw_flash_in_progress = false;
	netdev_put(module_fw->fw_update.dev, &module_fw->dev_tracker);
	release_firmware(module_fw->fw_update.fw);
	kfree(module_fw);
}

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
		INIT_WORK(&module_fw->work, module_flash_fw_work);
		break;
	default:
		NL_SET_ERR_MSG(extack,
			       "Module type does not support firmware flashing");
		return -EOPNOTSUPP;
	}

	return 0;
}

void ethnl_module_fw_flash_sock_destroy(struct ethnl_sock_priv *sk_priv)
{
	struct ethtool_module_fw_flash *work;

	spin_lock(&module_fw_flash_work_list_lock);
	list_for_each_entry(work, &module_fw_flash_work_list, list) {
		if (work->fw_update.dev == sk_priv->dev &&
		    work->fw_update.ntf_params.portid == sk_priv->portid) {
			work->fw_update.ntf_params.closed_sock = true;
			break;
		}
	}
	spin_unlock(&module_fw_flash_work_list_lock);
}

static int
module_flash_fw_schedule(struct net_device *dev, const char *file_name,
			 struct ethtool_module_fw_flash_params *params,
			 struct sk_buff *skb, struct genl_info *info)
{
	struct ethtool_cmis_fw_update_params *fw_update;
	struct ethtool_module_fw_flash *module_fw;
	int err;

	module_fw = kzalloc(sizeof(*module_fw), GFP_KERNEL);
	if (!module_fw)
		return -ENOMEM;

	fw_update = &module_fw->fw_update;
	fw_update->params = *params;
	err = request_firmware_direct(&fw_update->fw,
				      file_name, &dev->dev);
	if (err) {
		NL_SET_ERR_MSG(info->extack,
			       "Failed to request module firmware image");
		goto err_free;
	}

	err = module_flash_fw_work_init(module_fw, dev, info->extack);
	if (err < 0)
		goto err_release_firmware;

	dev->ethtool->module_fw_flash_in_progress = true;
	netdev_hold(dev, &module_fw->dev_tracker, GFP_KERNEL);
	fw_update->dev = dev;
	fw_update->ntf_params.portid = info->snd_portid;
	fw_update->ntf_params.seq = info->snd_seq;
	fw_update->ntf_params.closed_sock = false;

	err = ethnl_sock_priv_set(skb, dev, fw_update->ntf_params.portid,
				  ETHTOOL_SOCK_TYPE_MODULE_FW_FLASH);
	if (err < 0)
		goto err_release_firmware;

	err = module_flash_fw_work_list_add(module_fw, info);
	if (err < 0)
		goto err_release_firmware;

	schedule_work(&module_fw->work);

	return 0;

err_release_firmware:
	release_firmware(fw_update->fw);
err_free:
	kfree(module_fw);
	return err;
}

static int module_flash_fw(struct net_device *dev, struct nlattr **tb,
			   struct sk_buff *skb, struct genl_info *info)
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

	return module_flash_fw_schedule(dev, file_name, &params, skb, info);
}

static int ethnl_module_fw_flash_validate(struct net_device *dev,
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

	if (dev->ethtool->module_fw_flash_in_progress) {
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
	netdev_lock_ops(dev);
	ret = ethnl_ops_begin(dev);
	if (ret < 0)
		goto out_unlock;

	ret = ethnl_module_fw_flash_validate(dev, info->extack);
	if (ret < 0)
		goto out_unlock;

	ret = module_flash_fw(dev, tb, skb, info);

	ethnl_ops_complete(dev);

out_unlock:
	netdev_unlock_ops(dev);
	rtnl_unlock();
	ethnl_parse_header_dev_put(&req_info);
	return ret;
}

/* MODULE_FW_FLASH_NTF */

static int
ethnl_module_fw_flash_ntf_put_err(struct sk_buff *skb, char *err_msg,
				  char *sub_err_msg)
{
	int err_msg_len, sub_err_msg_len, total_len;
	struct nlattr *attr;

	if (!err_msg)
		return 0;

	err_msg_len = strlen(err_msg);
	total_len = err_msg_len + 2; /* For period and NUL. */

	if (sub_err_msg) {
		sub_err_msg_len = strlen(sub_err_msg);
		total_len += sub_err_msg_len + 2; /* For ", ". */
	}

	attr = nla_reserve(skb, ETHTOOL_A_MODULE_FW_FLASH_STATUS_MSG,
			   total_len);
	if (!attr)
		return -ENOMEM;

	if (sub_err_msg)
		sprintf(nla_data(attr), "%s, %s.", err_msg, sub_err_msg);
	else
		sprintf(nla_data(attr), "%s.", err_msg);

	return 0;
}

static void
ethnl_module_fw_flash_ntf(struct net_device *dev,
			  enum ethtool_module_fw_flash_status status,
			  struct ethnl_module_fw_flash_ntf_params *ntf_params,
			  char *err_msg, char *sub_err_msg,
			  u64 done, u64 total)
{
	struct sk_buff *skb;
	void *hdr;
	int ret;

	if (ntf_params->closed_sock)
		return;

	skb = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!skb)
		return;

	hdr = ethnl_unicast_put(skb, ntf_params->portid, ++ntf_params->seq,
				ETHTOOL_MSG_MODULE_FW_FLASH_NTF);
	if (!hdr)
		goto err_skb;

	ret = ethnl_fill_reply_header(skb, dev,
				      ETHTOOL_A_MODULE_FW_FLASH_HEADER);
	if (ret < 0)
		goto err_skb;

	if (nla_put_u32(skb, ETHTOOL_A_MODULE_FW_FLASH_STATUS, status))
		goto err_skb;

	ret = ethnl_module_fw_flash_ntf_put_err(skb, err_msg, sub_err_msg);
	if (ret < 0)
		goto err_skb;

	if (nla_put_uint(skb, ETHTOOL_A_MODULE_FW_FLASH_DONE, done))
		goto err_skb;

	if (nla_put_uint(skb, ETHTOOL_A_MODULE_FW_FLASH_TOTAL, total))
		goto err_skb;

	genlmsg_end(skb, hdr);
	genlmsg_unicast(dev_net(dev), skb, ntf_params->portid);
	return;

err_skb:
	nlmsg_free(skb);
}

void ethnl_module_fw_flash_ntf_err(struct net_device *dev,
				   struct ethnl_module_fw_flash_ntf_params *params,
				   char *err_msg, char *sub_err_msg)
{
	ethnl_module_fw_flash_ntf(dev, ETHTOOL_MODULE_FW_FLASH_STATUS_ERROR,
				  params, err_msg, sub_err_msg, 0, 0);
}

void
ethnl_module_fw_flash_ntf_start(struct net_device *dev,
				struct ethnl_module_fw_flash_ntf_params *params)
{
	ethnl_module_fw_flash_ntf(dev, ETHTOOL_MODULE_FW_FLASH_STATUS_STARTED,
				  params, NULL, NULL, 0, 0);
}

void
ethnl_module_fw_flash_ntf_complete(struct net_device *dev,
				   struct ethnl_module_fw_flash_ntf_params *params)
{
	ethnl_module_fw_flash_ntf(dev, ETHTOOL_MODULE_FW_FLASH_STATUS_COMPLETED,
				  params, NULL, NULL, 0, 0);
}

void
ethnl_module_fw_flash_ntf_in_progress(struct net_device *dev,
				      struct ethnl_module_fw_flash_ntf_params *params,
				      u64 done, u64 total)
{
	ethnl_module_fw_flash_ntf(dev,
				  ETHTOOL_MODULE_FW_FLASH_STATUS_IN_PROGRESS,
				  params, NULL, NULL, done, total);
}
