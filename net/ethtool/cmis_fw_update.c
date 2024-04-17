// SPDX-License-Identifier: GPL-2.0-only

#include <linux/ethtool.h>
#include <linux/firmware.h>

#include "common.h"
#include "module_fw.h"
#include "cmis.h"

struct cmis_fw_update_fw_mng_features {
	u8	start_cmd_payload_size;
	u16	max_duration_start;
	u16	max_duration_write;
	u16	max_duration_complete;
};

/* See section 9.4.2 "CMD 0041h: Firmware Management Features" in CMIS standard
 * revision 5.2.
 * struct cmis_cdb_fw_mng_features_rpl is a structured layout of the flat
 * array, ethtool_cmis_cdb_rpl::payload.
 */
struct cmis_cdb_fw_mng_features_rpl {
	u8	resv1;
	u8	resv2;
	u8	start_cmd_payload_size;
	u8	resv3;
	u8	read_write_len_ext;
	u8	write_mechanism;
	u8	resv4;
	u8	resv5;
	__be16	max_duration_start;
	__be16	resv6;
	__be16	max_duration_write;
	__be16	max_duration_complete;
	__be16	resv7;
};

#define CMIS_CDB_FW_WRITE_MECHANISM_LPL	0x01

static int
cmis_fw_update_fw_mng_features_get(struct ethtool_cmis_cdb *cdb,
				   struct net_device *dev,
				   struct cmis_fw_update_fw_mng_features *fw_mng)
{
	struct ethtool_cmis_cdb_cmd_args args = {};
	struct cmis_cdb_fw_mng_features_rpl *rpl;
	u8 flags = CDB_F_STATUS_VALID;
	int err;

	ethtool_cmis_cdb_check_completion_flag(cdb->cmis_rev, &flags);
	ethtool_cmis_cdb_compose_args(&args,
				      ETHTOOL_CMIS_CDB_CMD_FW_MANAGMENT_FEATURES,
				      NULL, 0, cdb->max_completion_time,
				      cdb->read_write_len_ext, 1000,
				      sizeof(*rpl), flags);

	err = ethtool_cmis_cdb_execute_cmd(dev, &args);
	if (err < 0) {
		ethnl_module_fw_flash_ntf_err(dev,
					      "FW Management Features command failed",
					      args.err_msg);
		return err;
	}

	rpl = (struct cmis_cdb_fw_mng_features_rpl *)args.req.payload;
	if (!(rpl->write_mechanism == CMIS_CDB_FW_WRITE_MECHANISM_LPL)) {
		ethnl_module_fw_flash_ntf_err(dev,
					      "Write LPL is not supported",
					      NULL);
		return  -EOPNOTSUPP;
	}

	/* Above, we used read_write_len_ext that we got from CDB
	 * advertisement. Update it with the value that we got from module
	 * features query, which is specific for Firmware Management Commands
	 * (IDs 0100h-01FFh).
	 */
	cdb->read_write_len_ext = rpl->read_write_len_ext;
	fw_mng->start_cmd_payload_size = rpl->start_cmd_payload_size;
	fw_mng->max_duration_start = be16_to_cpu(rpl->max_duration_start);
	fw_mng->max_duration_write = be16_to_cpu(rpl->max_duration_write);
	fw_mng->max_duration_complete = be16_to_cpu(rpl->max_duration_complete);

	return 0;
}

/* See section 9.7.2 "CMD 0101h: Start Firmware Download" in CMIS standard
 * revision 5.2.
 * struct cmis_cdb_start_fw_download_pl is a structured layout of the
 * flat array, ethtool_cmis_cdb_request::payload.
 */
struct cmis_cdb_start_fw_download_pl {
	__struct_group(cmis_cdb_start_fw_download_pl_h, head, /* no attrs */,
			__be32	image_size;
			__be32	resv1;
	);
	u8 vendor_data[ETHTOOL_CMIS_CDB_LPL_MAX_PL_LENGTH -
		sizeof(struct cmis_cdb_start_fw_download_pl_h)];
};

static int
cmis_fw_update_start_download(struct ethtool_cmis_cdb *cdb,
			      struct ethtool_module_fw_flash *module_fw,
			      struct cmis_fw_update_fw_mng_features *fw_mng)
{
	u8 vendor_data_size = fw_mng->start_cmd_payload_size;
	struct cmis_cdb_start_fw_download_pl pl = {};
	struct ethtool_cmis_cdb_cmd_args args = {};
	u8 lpl_len;
	int err;

	pl.image_size = cpu_to_be32(module_fw->fw->size);
	memcpy(pl.vendor_data, module_fw->fw->data, vendor_data_size);

	lpl_len = offsetof(struct cmis_cdb_start_fw_download_pl,
			   vendor_data[vendor_data_size]);

	ethtool_cmis_cdb_compose_args(&args,
				      ETHTOOL_CMIS_CDB_CMD_START_FW_DOWNLOAD,
				      (u8 *)&pl, lpl_len,
				      fw_mng->max_duration_start,
				      cdb->read_write_len_ext, 1000, 0,
				      CDB_F_COMPLETION_VALID | CDB_F_STATUS_VALID);

	err = ethtool_cmis_cdb_execute_cmd(module_fw->dev, &args);
	if (err < 0)
		ethnl_module_fw_flash_ntf_err(module_fw->dev,
					      "Start FW download command failed",
					      args.err_msg);

	return err;
}

/* See section 9.7.4 "CMD 0103h: Write Firmware Block LPL" in CMIS standard
 * revision 5.2.
 * struct cmis_cdb_write_fw_block_lpl_pl is a structured layout of the
 * flat array, ethtool_cmis_cdb_request::payload.
 */
struct cmis_cdb_write_fw_block_lpl_pl {
	__be32	block_address;
	u8 fw_block[ETHTOOL_CMIS_CDB_LPL_MAX_PL_LENGTH - sizeof(__be32)];
};

static int
cmis_fw_update_write_image(struct ethtool_cmis_cdb *cdb,
			   struct ethtool_module_fw_flash *module_fw,
			   struct cmis_fw_update_fw_mng_features *fw_mng)
{
	u8 start = fw_mng->start_cmd_payload_size;
	u32 image_size = module_fw->fw->size;
	u32 offset, max_block_size, max_lpl_len;
	int err;

	max_lpl_len = min_t(u32,
			    ethtool_cmis_get_max_payload_size(cdb->read_write_len_ext),
			    ETHTOOL_CMIS_CDB_LPL_MAX_PL_LENGTH);
	max_block_size =
		max_lpl_len - sizeof_field(struct cmis_cdb_write_fw_block_lpl_pl,
					   block_address);

	for (offset = start; offset < image_size; offset += max_block_size) {
		struct cmis_cdb_write_fw_block_lpl_pl pl = {
			.block_address = cpu_to_be32(offset - start),
		};
		struct ethtool_cmis_cdb_cmd_args args = {};
		u32 block_size, lpl_len;

		ethnl_module_fw_flash_ntf_in_progress(module_fw->dev,
						      offset - start,
						      image_size);
		block_size = min_t(u32, max_block_size, image_size - offset);
		memcpy(pl.fw_block, &module_fw->fw->data[offset], block_size);
		lpl_len = block_size +
			sizeof_field(struct cmis_cdb_write_fw_block_lpl_pl,
				     block_address);

		ethtool_cmis_cdb_compose_args(&args,
					      ETHTOOL_CMIS_CDB_CMD_WRITE_FW_BLOCK_LPL,
					      (u8 *)&pl, lpl_len,
					      fw_mng->max_duration_write,
					      cdb->read_write_len_ext, 1, 0,
					      CDB_F_COMPLETION_VALID | CDB_F_STATUS_VALID);

		err = ethtool_cmis_cdb_execute_cmd(module_fw->dev, &args);
		if (err < 0) {
			ethnl_module_fw_flash_ntf_err(module_fw->dev,
						      "Write FW block LPL command failed",
						      args.err_msg);
			return err;
		}
	}

	return 0;
}

static int
cmis_fw_update_complete_download(struct ethtool_cmis_cdb *cdb,
				 struct net_device *dev,
				 struct cmis_fw_update_fw_mng_features *fw_mng)
{
	struct ethtool_cmis_cdb_cmd_args args = {};
	int err;

	ethtool_cmis_cdb_compose_args(&args,
				      ETHTOOL_CMIS_CDB_CMD_COMPLETE_FW_DOWNLOAD,
				      NULL, 0, fw_mng->max_duration_complete,
				      cdb->read_write_len_ext, 1000, 0,
				      CDB_F_COMPLETION_VALID | CDB_F_STATUS_VALID);

	err = ethtool_cmis_cdb_execute_cmd(dev, &args);
	if (err < 0)
		ethnl_module_fw_flash_ntf_err(dev,
					      "Complete FW download command failed",
					      args.err_msg);

	return err;
}

static int
cmis_fw_update_download_image(struct ethtool_cmis_cdb *cdb,
			      struct ethtool_module_fw_flash *module_fw,
			      struct cmis_fw_update_fw_mng_features *fw_mng)
{
	int err;

	err = cmis_fw_update_start_download(cdb, module_fw, fw_mng);
	if (err < 0)
		return err;

	err = cmis_fw_update_write_image(cdb, module_fw, fw_mng);
	if (err < 0)
		return err;

	err = cmis_fw_update_complete_download(cdb, module_fw->dev, fw_mng);
	if (err < 0)
		return err;

	return 0;
}

enum {
	CMIS_MODULE_LOW_PWR	= 1,
	CMIS_MODULE_READY	= 3,
};

static bool module_is_ready(u8 data)
{
	u8 state = (data >> 1) & 7;

	return state == CMIS_MODULE_READY || state == CMIS_MODULE_LOW_PWR;
}

#define CMIS_MODULE_READY_MAX_DURATION_USEC	1000
#define CMIS_MODULE_STATE_OFFSET		3

static int
cmis_fw_update_wait_for_module_state(struct ethtool_module_fw_flash *module_fw,
				     u8 flags)
{
	u8 state;

	return ethtool_cmis_wait_for_cond(module_fw->dev, flags,
					  CDB_F_MODULE_STATE_VALID,
					  CMIS_MODULE_READY_MAX_DURATION_USEC,
					  CMIS_MODULE_STATE_OFFSET,
					  module_is_ready, NULL, &state);
}

/* See section 9.7.10 "CMD 0109h: Run Firmware Image" in CMIS standard
 * revision 5.2.
 * struct cmis_cdb_run_fw_image_pl is a structured layout of the flat
 * array, ethtool_cmis_cdb_request::payload.
 */
struct cmis_cdb_run_fw_image_pl {
	u8 resv1;
	u8 image_to_run;
	u16 delay_to_reset;
};

static int cmis_fw_update_run_image(struct ethtool_cmis_cdb *cdb,
				    struct ethtool_module_fw_flash *module_fw)
{
	struct ethtool_cmis_cdb_cmd_args args = {};
	struct cmis_cdb_run_fw_image_pl pl = {0};
	int err;

	ethtool_cmis_cdb_compose_args(&args, ETHTOOL_CMIS_CDB_CMD_RUN_FW_IMAGE,
				      (u8 *)&pl, sizeof(pl),
				      cdb->max_completion_time,
				      cdb->read_write_len_ext, 1000, 0,
				      CDB_F_MODULE_STATE_VALID);

	err = ethtool_cmis_cdb_execute_cmd(module_fw->dev, &args);
	if (err < 0) {
		ethnl_module_fw_flash_ntf_err(module_fw->dev,
					      "Run image command failed",
					      args.err_msg);
		return err;
	}

	err = cmis_fw_update_wait_for_module_state(module_fw, args.flags);
	if (err < 0)
		ethnl_module_fw_flash_ntf_err(module_fw->dev,
					      "Module is not ready on time after reset",
					      NULL);

	return err;
}

static int
cmis_fw_update_commit_image(struct ethtool_cmis_cdb *cdb,
			    struct ethtool_module_fw_flash *module_fw)
{
	struct ethtool_cmis_cdb_cmd_args args = {};
	int err;

	ethtool_cmis_cdb_compose_args(&args,
				      ETHTOOL_CMIS_CDB_CMD_COMMIT_FW_IMAGE,
				      NULL, 0, cdb->max_completion_time,
				      cdb->read_write_len_ext, 1000, 0,
				      CDB_F_COMPLETION_VALID | CDB_F_STATUS_VALID);

	err = ethtool_cmis_cdb_execute_cmd(module_fw->dev, &args);
	if (err < 0)
		ethnl_module_fw_flash_ntf_err(module_fw->dev,
					      "Commit image command failed",
					      args.err_msg);

	return err;
}

static int cmis_fw_update_reset(struct net_device *dev)
{
	__u32 reset_data = ETH_RESET_PHY;

	return dev->ethtool_ops->reset(dev, &reset_data);
}

void ethtool_cmis_fw_update(struct work_struct *work)
{
	struct cmis_fw_update_fw_mng_features fw_mng = {0};
	struct ethtool_module_fw_flash *module_fw;
	struct ethtool_cmis_cdb *cdb;
	int err;

	module_fw = container_of(work, struct ethtool_module_fw_flash, work);

	cdb = ethtool_cmis_cdb_init(module_fw->dev, &module_fw->params);
	if (IS_ERR(cdb))
		goto err_send_ntf;

	ethnl_module_fw_flash_ntf_start(module_fw->dev);

	err = cmis_fw_update_fw_mng_features_get(cdb, module_fw->dev, &fw_mng);
	if (err < 0)
		goto err_cdb_fini;

	err = cmis_fw_update_download_image(cdb, module_fw, &fw_mng);
	if (err < 0)
		goto err_cdb_fini;

	err = cmis_fw_update_run_image(cdb, module_fw);
	if (err < 0)
		goto err_cdb_fini;

	/* The CDB command "Run Firmware Image" resets the firmware, so the new
	 * one might have different settings.
	 * Free the old CDB instance, and init a new one.
	 */
	ethtool_cmis_cdb_fini(cdb);

	cdb = ethtool_cmis_cdb_init(module_fw->dev, &module_fw->params);
	if (IS_ERR(cdb))
		goto err_send_ntf;

	err = cmis_fw_update_commit_image(cdb, module_fw);
	if (err < 0)
		goto err_cdb_fini;

	err = cmis_fw_update_reset(module_fw->dev);
	if (err < 0)
		goto err_cdb_fini;

	ethnl_module_fw_flash_ntf_complete(module_fw->dev);
	ethtool_cmis_cdb_fini(cdb);
	goto out;

err_cdb_fini:
	ethtool_cmis_cdb_fini(cdb);
err_send_ntf:
	ethnl_module_fw_flash_ntf_err(module_fw->dev, NULL, NULL);
out:
	module_fw->dev->module_fw_flash_in_progress = false;
	netdev_put(module_fw->dev, &module_fw->dev_tracker);
	release_firmware(module_fw->fw);
	kfree(module_fw);
}
