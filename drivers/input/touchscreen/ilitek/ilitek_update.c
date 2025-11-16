// SPDX-License-Identifier: GPL-2.0
/*
 * This file is part of ILITEK CommonFlow
 *
 * Copyright (c) 2022 ILI Technology Corp.
 * Copyright (c) 2022 Luca Hsu <luca_hsu@ilitek.com>
 * Copyright (c) 2022 Joe Hung <joe_hung@ilitek.com>
 */

#include "ilitek_update.h"

#ifdef _WIN32
/* packed below structures by 1 byte */
#pragma pack(1)
#endif


#ifdef _WIN32
#pragma pack()
#endif

#ifndef __KERNEL__
static int hex_to_bin(u8 ch)
{
	u8 cu = ch & 0xdf;
	return -1 +
		((ch - '0' +  1) & (unsigned)((ch - '9' - 1) &
		('0' - 1 - ch)) >> 8) +
		((cu - 'A' + 11) & (unsigned)((cu - 'F' - 1) &
		('A' - 1 - cu)) >> 8);
}

static int hex2bin(u8 *dst, const u8 *src, size_t count)
{
	int hi = 0, lo = 0;

	while (count--) {
		hi = hex_to_bin(*src++);
		lo = hex_to_bin(*src++);
		if (hi < 0 || lo < 0) {
			TP_ERR(NULL, "hex_to_bin failed, hi: %d, lo: %d\n",
				hi, lo);
			return -EINVAL;
		}

		*dst++ = (hi << 4) | lo;
	}
	return 0;
}
#endif

static u32 get_tag_addr(u32 start, u32 end,
			const u8 *buf, unsigned int buf_size,
			const u8 *tag, unsigned int tag_size)
{
	unsigned int i;

	for (i = start; i <= end - tag_size && i < buf_size - tag_size; i++) {
		if (!memcmp(buf + i, tag, tag_size))
			return i + tag_size + 1;
	}

	return end;
}

static u32 get_endaddr(u32 start, u32 end, const u8 *buf,
		       unsigned int buf_size, bool is_AP)
{
	u32 addr;
	u8 tag[32];
	const u8 ap_tag[] = "ILITek AP CRC   ";
	const u8 blk_tag[] = "ILITek END TAG  ";

	_memset(tag, 0xFF, sizeof(tag));
	_memcpy(tag + 16, (is_AP) ? ap_tag : blk_tag, 16);

	addr = get_tag_addr(start, end, buf, buf_size, tag, sizeof(tag));
	TP_DBG(NULL, "find tag in start/end: 0x%x/0x%x, tag addr: 0x%x\n",
		start, end, addr);

	return addr;
}

static int decode_mm(struct ilitek_fw_handle *fw, u32 addr,
		     u8 *buf, u32 buf_size)
{
	u8 i;
	union mapping_info *mapping;

	TP_INFO(NULL, "------------Memory Mapping information------------\n");
	TP_INFO(NULL, "memory-mapping-info addr: 0x%x\n", addr);

	mapping = (union mapping_info *)(buf + addr);
	_memset(fw->file.ic_name, 0, sizeof(fw->file.ic_name));

	switch (mapping->mapping_ver[2]) {
	case 0x2:
		_memcpy(fw->file.ic_name, mapping->ic_name,
			sizeof(mapping->ic_name));
		break;
	default:
	case 0x1:
		_sprintf(fw->file.ic_name, 0, "%02x%02x",
			mapping->ic_name[1], mapping->ic_name[0]);
		break;
	}

	if (!strcmp(fw->file.ic_name, "2133"))
		_sprintf(fw->file.ic_name, 0, "2132S");

	if (fw->dev && strcmp(fw->dev->mcu_info.ic_name, fw->file.ic_name)) {
		TP_ERR(fw->dev->id, "IC: " PFMT_C8 ", Firmware File: " PFMT_C8 " not matched\n",
			fw->dev->mcu_info.ic_name, fw->file.ic_name);
		return -EINVAL;
	}

	TP_MSG(NULL, "Hex Mapping Ver.: 0x%x\n",
		le32(mapping->mapping_ver, 3));
	TP_MSG(NULL, "Hex Protocol: 0x%x\n",
		le32(mapping->protocol_ver, 3));
	TP_MSG(NULL, "Hex MCU Ver.: " PFMT_C8 "\n", fw->file.ic_name);

	_memset(fw->file.fw_ver, 0, sizeof(fw->file.fw_ver));
	fw->file.fwid = 0xffff;

	fw->file.mm_addr = addr;
	switch (addr) {
	case 0x4038:
	case 0x4020:
		fw->file.mm_size = 128;
		fw->file.fw_ver[0] = mapping->_lego.fw_ver[3];
		fw->file.fw_ver[1] = mapping->_lego.fw_ver[2];
		fw->file.fw_ver[2] = mapping->_lego.fw_ver[1];
		fw->file.fw_ver[3] = mapping->_lego.fw_ver[0];
		fw->file.fw_ver[4] = buf[0x2C007];
		fw->file.fw_ver[5] = buf[0x2C006];
		fw->file.fw_ver[6] = buf[0x2C005];
		fw->file.fw_ver[7] = buf[0x2C004];

		fw->file.fwid = mapping->_lego.fwid;
		break;
	case 0x3020:
		fw->file.mm_size = 128;
		fw->file.fw_ver[0] = mapping->_lego.fw_ver[3];
		fw->file.fw_ver[1] = mapping->_lego.fw_ver[2];
		fw->file.fw_ver[2] = mapping->_lego.fw_ver[1];
		fw->file.fw_ver[3] = mapping->_lego.fw_ver[0];
		fw->file.fw_ver[4] = buf[0x3C007];
		fw->file.fw_ver[5] = buf[0x3C006];
		fw->file.fw_ver[6] = buf[0x3C005];
		fw->file.fw_ver[7] = buf[0x3C004];

		fw->file.fwid = mapping->_lego.fwid;
		break;
	case 0x2020:
		fw->file.mm_size = 132;
		fw->file.fw_ver[0] = buf[0x2033];
		fw->file.fw_ver[1] = buf[0x2032];
		fw->file.fw_ver[2] = buf[0x2031];
		fw->file.fw_ver[3] = buf[0x2030];
		fw->file.fw_ver[4] = buf[0xF004];
		fw->file.fw_ver[5] = buf[0xF005];
		fw->file.fw_ver[6] = buf[0xF006];
		fw->file.fw_ver[7] = buf[0xF007];

		/* for V3 251x IC, get AP crc and DF checksum */
		fw->file.blocks[0].check = get_crc(fw->file.blocks[0].start,
			fw->file.blocks[0].end - 1, buf, buf_size);

		if (fw->file.blocks[1].end > fw->file.blocks[1].start) {
			fw->file.blocks[1].check = get_checksum(
				fw->file.blocks[1].start,
				fw->file.blocks[1].end + 1, buf, buf_size);
		}
		break;

	case 0x500:
		fw->file.mm_size = 132;
		fw->file.fw_ver[0] = buf[0x52D];
		fw->file.fw_ver[1] = buf[0x52C];
		fw->file.fw_ver[2] = buf[0x52B];
		fw->file.fw_ver[3] = buf[0x52A];
		fw->file.fw_ver[4] = buf[0x1F404];
		fw->file.fw_ver[5] = buf[0x1F405];
		fw->file.fw_ver[6] = buf[0x1F406];
		fw->file.fw_ver[7] = buf[0x1F407];

		/* for V3 231x IC, get AP checksum and DF checksum */
		fw->file.blocks[0].check = get_checksum(
			fw->file.blocks[0].start,
			fw->file.blocks[0].end + 1, buf, buf_size);
		if (fw->file.blocks[1].end > fw->file.blocks[1].start) {
			fw->file.blocks[1].check = get_checksum(
				fw->file.blocks[1].start,
				fw->file.blocks[1].end + 1, buf, buf_size);
		}
		break;
	default:
		fw->file.mm_size = 0;
		break;
	}

	TP_MSG(NULL, "file fwid: 0x%04x\n", fw->file.fwid);

	TP_INFO(NULL, "File FW Version: %02x-%02x-%02x-%02x\n",
		fw->file.fw_ver[0], fw->file.fw_ver[1],
		fw->file.fw_ver[2], fw->file.fw_ver[3]);
	TP_INFO(NULL, "File Customer Version: %02x-%02x-%02x-%02x\n",
		fw->file.fw_ver[4], fw->file.fw_ver[5],
		fw->file.fw_ver[6], fw->file.fw_ver[7]);

	if (le32(mapping->mapping_ver, 3) < 0x10000)
		goto memory_mapping_end;

	TP_INFO(NULL, "File Tuning Version: %02x-%02x-%02x-%02x\n",
		mapping->_lego.tuning_ver[3], mapping->_lego.tuning_ver[2],
		mapping->_lego.tuning_ver[1], mapping->_lego.tuning_ver[0]);

	if (mapping->_lego.block_num > ARRAY_SIZE(fw->file.blocks)) {
		TP_ERR(NULL, "Unexpected block num: " PFMT_U8 " > %u\n",
			mapping->_lego.block_num,
			(unsigned int)ARRAY_SIZE(fw->file.blocks));
		goto memory_mapping_end;
	}

	fw->file.block_num = mapping->_lego.block_num;

	TP_MSG(NULL, "Total " PFMT_U8 " blocks\n", fw->file.block_num);
	for (i = 0; i < fw->file.block_num; i++) {
		fw->file.blocks[i].start =
			le32(mapping->_lego.blocks[i].addr, 3);
		fw->file.blocks[i].end = (i == fw->file.block_num - 1) ?
			le32(mapping->_lego.end_addr, 3) :
			le32(mapping->_lego.blocks[i + 1].addr, 3);

		/*
		 * get end addr. of block,
		 * i.e. address of block's final byte of crc.
		 */
		fw->file.blocks[i].end = get_endaddr(
			fw->file.blocks[i].start, fw->file.blocks[i].end,
			buf, buf_size, i == 0);

		fw->file.blocks[i].check = get_crc(fw->file.blocks[i].start,
			fw->file.blocks[i].end - 1,
			buf, buf_size);

		TP_MSG(NULL, "Block[%u], start:0x%x end:0x%x, crc:0x%x\n",
			i, fw->file.blocks[i].start, fw->file.blocks[i].end,
			fw->file.blocks[i].check);
	}

memory_mapping_end:
	TP_INFO(NULL, "--------------------------------------------------\n");

	return 0;
}

static int decode_hex(struct ilitek_fw_handle *fw, u8 *hex,
		      u32 start, u32 end,
		      u8 *buf, u32 buf_size)
{
	int error;
	u8 info[4], data[16];
	unsigned int i, len, addr, type, exaddr = 0;
	u32 mapping_info_addr = 0;

	/* m2v hex has another block at the end of hex file */
	u8 j = (fw->m2v) ? fw->file.block_num : 0;

	fw->file.blocks[j].start = (~0U);
	fw->file.blocks[j].end = 0x0;
	fw->file.blocks[j].check = 0x0;
	fw->file.blocks[j + 1].start = (~0U);
	fw->file.blocks[j + 1].end = 0x0;
	fw->file.blocks[j + 1].check = 0x0;

	for (i = start; i < end; i++) {
		/* filter out non-hexadecimal characters */
		if (hex_to_bin(hex[i]) < 0)
			continue;

		error = hex2bin(info, hex + i, sizeof(info));
		if (error < 0)
			return error;

		len = info[0];
		addr = be32(info + 1, 2);
		type = info[3];

		error = hex2bin(data, hex + i + 8, len);
		if (error < 0)
			return error;

		switch (type) {
		case 0xAC:
			mapping_info_addr = be32(data, len);
			break;

		case 0xAD:
			fw->file.blocks[1].start = be32(data, len);
			_memset(buf + fw->file.blocks[1].start, 0, 0x1000);
			break;

		case 0xBA:
			if (be32(data, len) != 2U)
				break;

			TP_MSG(NULL, "start to decode M2V part of hex file\n");
			fw->m2v = true;
			////Reed Add : 20230721��������2326��������Decode_mm,�ټ�������M2V���֡���
			if (mapping_info_addr)
				decode_mm(fw, mapping_info_addr, buf, buf_size);
			return decode_hex(fw, hex, i + 10 + len * 2 + 1, end,
					  fw->m2v_buf, ILITEK_FW_BUF_SIZE);

		case 0x01:
			goto success_return;

		case 0x02:
			exaddr = be32(data, len) << 4;
			break;

		case 0x04:
			exaddr = be32(data, len) << 16;
			break;

		case 0x05:
			TP_MSG(NULL, "hex data type: 0x%x, start linear address: 0x%x\n",
				type, be32(data, len));
			break;

		case 0x00:
			addr += exaddr;

			if (addr + len > buf_size) {
				TP_ERR(NULL, "hex addr: 0x%x, buf size: 0x%x OOB\n",
					addr + len, buf_size);
				return -ENOBUFS;
			}
			_memcpy(buf + addr, data, len);

			fw->file.blocks[j].start =
				MIN(fw->file.blocks[j].start, addr);

			if (addr + len < fw->file.blocks[j + 1].start) {
				fw->file.blocks[j].end =
					MAX(fw->file.blocks[j].end,
					    addr + len - 1);
				fw->file.blocks[j].check += get_checksum(
					0, len, data, sizeof(data));
			} else {
				fw->file.blocks[j + 1].end =
					MAX(fw->file.blocks[j + 1].end,
					    addr + len - 1);
				fw->file.blocks[j + 1].check += get_checksum(
					0, len, data, sizeof(data));
			}

			break;
		default:
			TP_ERR(NULL, "unexpected type:0x%x in hex, len:%u, addr:0x%x\n",
				type, len, addr);
			return -EINVAL;
		}

		i = i + 10 + len * 2;
	}

success_return:
	if (fw->m2v)
		fw->m2v_checksum = fw->file.blocks[fw->file.block_num].check;
	if (mapping_info_addr)
		return decode_mm(fw, mapping_info_addr, fw->file.buf, buf_size);

	return 0;
}

static int decode_bin(struct ilitek_fw_handle *fw,
		      u8 *bin, u32 bin_size,
		      u8 *buf, u32 buf_size)
{
	int error;
	struct ilitek_ts_device *dev = fw->dev;
	u32 mapping_info_addr;

	if (!dev) {
		TP_ERR(NULL, "offline decode bin file is not supported\n");
		return -EINVAL;
	}

	if (bin_size > buf_size) {
		TP_ERR(dev->id, "bin file size: 0x%x, buf size: 0x%x OOB\n",
			bin_size, buf_size);
		return -ENOBUFS;
	}
	_memcpy(buf, bin, bin_size);

	error = api_protocol_set_cmd(dev, GET_PTL_VER, NULL);
	if (error < 0)
		return error;
	error = api_protocol_set_cmd(dev, GET_MCU_VER, NULL);
	if (error < 0)
		return error;

	switch (dev->protocol.flag) {
	case PTL_V6:
		mapping_info_addr = dev->mcu_info.mm_addr;
		break;

	case PTL_V3:
		/*
		 * For 231x: AP checksum and DF checksum, DF start addr: 0x1f000
		 * For 251x: AP crc and DF checksum, DF start addr: 0xf000
		 */
		if (is_231x(dev)) {
			mapping_info_addr = 0x500;

			fw->file.blocks[1].start = 0x1f000;
			fw->file.blocks[1].end = bin_size - 1;
			fw->file.blocks[0].start = 0x0;

			/* +2 to get end addr. of last byte of 4 bytes checksum */
			fw->file.blocks[0].end =
				get_endaddr(fw->file.blocks[0].start,
					    fw->file.blocks[1].start,
					    bin, bin_size, true) + 2;

		} else {
			mapping_info_addr = 0x2020;
			fw->file.blocks[1].start = 0xf000;
			fw->file.blocks[1].end = bin_size - 1;
			fw->file.blocks[0].start = 0x2000;
			fw->file.blocks[0].end =
				get_endaddr(fw->file.blocks[0].start,
					    fw->file.blocks[1].start,
					    bin, bin_size, true);
		}

		break;

	default:
		return -EINVAL;
	}

	/*
	 * take the whole "buf" into decode_mm, "buf" should be
	 * properly initialized, and the size should be
	 * larger than "bin", which reduce OOB issue.
	 */
	return decode_mm(fw, mapping_info_addr, buf, buf_size);
}

#ifdef ILITEK_BOOT_UPDATE
#include "ilitek_fw.h"

static int decode_ili(struct ilitek_fw_handle *fw,
		      u8 *buf, u32 buf_size)
{
	struct ilitek_ts_device *dev = fw->dev;
	u8 *ili_buf = CTPM_FW;
	int size = sizeof(CTPM_FW);
	u8 id;

#if defined(ILITEK_BOOT_UPDATE_ILI_VER)
	switch (__ili_select_type__) {
	case ili_by_sensor_id:
		if (!support_sensor_id(dev)) {
			TP_WARN(dev->id,
				"protocol: 0x%x, mode: 0x" PFMT_X8 ", sensor-id not supported, take default fw(id: 0x%x)\n",
				dev->protocol.ver, dev->ic[0].mode,
				ILITEK_BOOT_UPDATE_DEF_ID);
			fw->file.id = ILITEK_BOOT_UPDATE_DEF_ID;
			break;
		}

		id = dev->sensor.id & dev->setting.sensor_id_mask;
		if (id >= ARRAY_SIZE(ili_arr)) {
			TP_ERR(dev->id, "invalid sensor id: " PFMT_U8 " >= %d\n",
				dev->sensor.id, (int)ARRAY_SIZE(ili_arr));
			return -EINVAL;
		}

		fw->file.id = id;
		ili_buf = ili_arr[id].buf;
		size = ili_arr[id].size;
		break;
	case ili_default:
		break;
	default:
		TP_ERR(dev->id, "unexpected ili-select-type: %u\n",
			__ili_select_type__);
		return -EINVAL;
	}
#endif

	if (!ili_buf || size < 32)
		return -EINVAL;

	fw->setting.fw_ver_check = true;
	fw->setting.fw_ver_policy = 0;
	_memcpy(fw->setting.fw_ver, ili_buf + 18, 8);

	TP_MSG_ARR(dev->id, "IC  fw ver:", TYPE_U8, 8, dev->fw_ver);
	TP_MSG_ARR(dev->id, "Hex fw ver:", TYPE_U8, 8, fw->setting.fw_ver);

	return decode_bin(fw, ili_buf + 32, size - 32, buf, buf_size);
}
#endif

static bool need_retry(struct ilitek_fw_handle *fw)
{
	struct ilitek_ts_device *dev = fw->dev;
	u32 id;

#if defined(ILITEK_BOOT_UPDATE) && defined(ILITEK_BOOT_UPDATE_ILI_VER)
	if (fw->file.type != fw_ili)
		return false;

	switch (__ili_select_type__) {
	case ili_by_sensor_id:
		id = dev->sensor.id & dev->setting.sensor_id_mask;
		if (id == fw->file.id)
			break;

		/* reload correct fw if sensor-id not matched with default fw */
		TP_MSG(dev->id, "sensor id: 0x%x, file: 0x%x not matched, "
			"reload fw again\n",
			id, fw->file.id);

		if (ilitek_update_load_fw(fw, "ilitek.ili") < 0) {
			TP_ERR(dev->id, "reload ilitek.ili failed\n");
			return false;
		}

		/* MUST DISABLE fw ver check for the second try */
		fw->setting.fw_ver_check = false;

		return true;
	default:
		break;
	}
#endif

	UNUSED(dev);
	UNUSED(id);

	return false;
}

static int decode_firmware(struct ilitek_fw_handle *fw, WCHAR *filename)
{
	int error;
	int size = 0;
	u8 *buf;
	WCHAR *file_ext;

	/* initialization */
	_memset(fw->file.buf, 0xFF, fw->file.buf_size);
	_memset(fw->m2v_buf, 0xFF, ILITEK_FW_BUF_SIZE);
	fw->m2v = false;
	/*
	 * set block num 2 for V3 AP and Data Flash as default,
	 * for V6, block num would be updated after decoding memory mapping.
	 */
	fw->file.block_num = 2;
	fw->file.blocks[0].start = (~0U);
	fw->file.blocks[0].end = 0x0;
	fw->file.blocks[0].check = 0x0;
	fw->file.blocks[1].start = (~0U);
	fw->file.blocks[1].end = 0x0;
	fw->file.blocks[1].check = 0x0;

	TP_MSG(NULL, "start to read fw file: " PFMT_C16 "\n", filename);

	buf = (u8 *)CALLOC(ILITEK_FW_FILE_SIZE, 1);
	if (!buf)
		return -ENOMEM;

	file_ext = WCSRCHR(filename, '.');
	if (!file_ext)
		return -ENOENT;

	/* no need to read .ili file */
	if (WCSCASECMP(file_ext, ".ili")) {
		if (!fw->cb.read_fw) {
			error = -EFAULT;
			TP_ERR(NULL, "read fw callback not registered\n");
			goto err_free;
		}

		size = fw->cb.read_fw(filename, buf, ILITEK_FW_FILE_SIZE,
				      fw->_private);

		error = size;
		if (error < 0) {
			TP_ERR(NULL, "read fw file: " PFMT_C16 " failed, err: %d\n",
				filename, error);
			goto err_free;
		}
	}

	if (!WCSCASECMP(file_ext, ".hex")) {
		fw->file.type = fw_hex;
		error = decode_hex(fw, buf, 0, size, fw->file.buf,
				   fw->file.buf_size);
	} else if (!WCSCASECMP(file_ext, ".bin")) {
		fw->file.type = fw_bin;
		error = decode_bin(fw, buf, size, fw->file.buf,
				   fw->file.buf_size);
	}
#ifdef ILITEK_BOOT_UPDATE
	else if (!WCSCASECMP(file_ext, ".ili")) {
		fw->file.type = fw_ili;
		error = decode_ili(fw, fw->file.buf, fw->file.buf_size);
	}
#endif
	else {
		error = -EINVAL;
	}

err_free:
	CFREE(buf);

	return error;
}

static bool need_fw_update_v3(struct ilitek_fw_handle *fw)
{
	struct ilitek_ts_device *dev = fw->dev;
	bool fw_file_has_data_flash =
		(fw->file.blocks[1].start < fw->file.blocks[1].end);
	bool need = false;

	TP_INFO(dev->id, "------------V3 AP/DF Info.------------\n");

	fw->file.blocks[0].check_match = (fw->setting.force_update) ?
		false : dev->ic[0].crc[0] == fw->file.blocks[0].check;

	TP_INFO(dev->id, "AP block Start/End Addr.: 0x%x/0x%x, IC/File Checksum: 0x%x/0x%x " PFMT_C8 "\n",
		fw->file.blocks[0].start, fw->file.blocks[0].end,
		dev->ic[0].crc[0], fw->file.blocks[0].check,
		(fw->file.blocks[0].check_match) ? "matched" : "not matched");

	fw->file.blocks[1].check_match = true;
	if (fw_file_has_data_flash) {
		fw->file.blocks[1].check_match = (fw->setting.force_update) ?
			false : dev->ic[0].crc[1] == fw->file.blocks[1].check;

		TP_INFO(dev->id, "DF block Start/End Addr.: 0x%x/0x%x, IC/File Checksum: 0x%x/0x%x " PFMT_C8 "\n",
			fw->file.blocks[1].start, fw->file.blocks[1].end,
			dev->ic[0].crc[1], fw->file.blocks[1].check,
			(fw->file.blocks[1].check_match) ?
			"matched" : "not matched");
	} else if (!is_231x(dev)) {
		/*
		 * for 251x ICs, if no data flash in fw file,
		 * need to switch to BL mode then erase data flash forcely.
		 */
		need = true;
	}

	TP_INFO(dev->id, "--------------------------------------\n");

	need |= (!fw->file.blocks[0].check_match ||
		 !fw->file.blocks[1].check_match);

	if (dev->ic[0].mode == BL_MODE)
		return true;

	return need;
}

static bool need_fw_update_v6(struct ilitek_fw_handle *fw)
{
	struct ilitek_ts_device *dev = fw->dev;
	u8 i;
	bool need = false;

	TP_INFO(dev->id, "------------Lego Block Info.------------\n");

	for (i = 0; i < fw->file.block_num; i++) {
		dev->ic[0].crc[i] = api_get_block_crc_by_addr(dev,
			CRC_CALCULATE, fw->file.blocks[i].start,
			fw->file.blocks[i].end);
	}

	for (i = 0; i < fw->file.block_num; i++) {
		fw->file.blocks[i].check_match = (fw->setting.force_update) ?
			false : (dev->ic[0].crc[i] == fw->file.blocks[i].check);

		need = (!fw->file.blocks[i].check_match) ? true : need;

		TP_INFO(dev->id, "Block[" PFMT_U8 "]: Start/End Addr.: 0x%x/0x%x, IC/File CRC: 0x%x/0x%x " PFMT_C8 "\n",
			i, fw->file.blocks[i].start, fw->file.blocks[i].end,
			dev->ic[0].crc[i], fw->file.blocks[i].check,
			(fw->file.blocks[i].check_match) ?
			"matched" : "not matched");
	}

	/* check BL mode firstly before AP-cmd related varaible, ex: ic_num */
	if (dev->ic[0].mode == BL_MODE) {
		need = true;
		goto force_return;
	}

	for (i = 1; i < dev->tp_info.ic_num; i++) {
		TP_INFO(dev->id, "Master/Slave[" PFMT_U8 "] CRC: 0x%x/0x%x " PFMT_C8 ", Slave Mode: 0x" PFMT_X8 " " PFMT_C8 "\n",
			i, fw->file.blocks[0].check, dev->ic[i].crc[0],
			(fw->file.blocks[0].check == dev->ic[i].crc[0]) ?
			"matched" : "not matched",
			dev->ic[i].mode, dev->ic[i].mode_str);

		if (dev->ic[i].crc[0] == fw->file.blocks[0].check &&
			dev->ic[i].mode == AP_MODE)
			continue;
		need = true;
	}

	if (fw->m2v) {
		api_access_slave(dev, 0x80, CMD_GET_AP_CRC, &fw->m2v_checksum);

		fw->file.blocks[fw->file.block_num].check_match =
			fw->file.blocks[fw->file.block_num].check ==
			fw->m2v_checksum;
		TP_INFO(dev->id, "M2V IC/File Checksum: 0x%x/0x%x " PFMT_C8 "\n",
			fw->m2v_checksum,
			fw->file.blocks[fw->file.block_num].check,
			(fw->file.blocks[fw->file.block_num].check_match) ?
			"matched" : "not matched");

		fw->m2v_need_update =
			!fw->file.blocks[fw->file.block_num].check_match ||
			fw->setting.force_update;
		need |= fw->m2v_need_update;
	}

force_return:
	TP_INFO(dev->id, "----------------------------------------\n");

	return need;
}

static bool need_fw_update(struct ilitek_fw_handle *fw)
{
	struct ilitek_ts_device *dev = fw->dev;
	bool need = false;

	struct ilitek_fw_settings *set = &fw->setting;
	u64 dev_fw_ver, file_fw_ver;

	if (dev->protocol.flag == PTL_V3)
		need = need_fw_update_v3(fw);
	else if (dev->protocol.flag == PTL_V6)
		need = need_fw_update_v6(fw);

	if (fw->cb.update_fw_ic_info)
		fw->cb.update_fw_ic_info(false,
					 dev->fw_ver, dev->ic[0].crc,
					 fw->file.block_num, fw->_private);

	if (set->force_update)
		return true;
	else if (set->fw_check_only)
		return false;

	if (set->fw_ver_check && dev->ic[0].mode == AP_MODE) {
		TP_INFO(dev->id, "IC FW version: %02x-%02x-%02x-%02x-%02x-%02x-%02x-%02x\n",
			dev->fw_ver[0], dev->fw_ver[1], dev->fw_ver[2],
			dev->fw_ver[3], dev->fw_ver[4], dev->fw_ver[5],
			dev->fw_ver[6], dev->fw_ver[7]);
		TP_INFO(dev->id, "File FW version: %02x-%02x-%02x-%02x-%02x-%02x-%02x-%02x\n",
			set->fw_ver[0], set->fw_ver[1],
			set->fw_ver[2], set->fw_ver[3],
			set->fw_ver[4], set->fw_ver[5],
			set->fw_ver[6], set->fw_ver[7]);

		dev_fw_ver =
			U82U64(dev->fw_ver[0], 7) + U82U64(dev->fw_ver[1], 6) +
			U82U64(dev->fw_ver[2], 5) + U82U64(dev->fw_ver[3], 4) +
			U82U64(dev->fw_ver[4], 3) + U82U64(dev->fw_ver[5], 2) +
			U82U64(dev->fw_ver[6], 1) + U82U64(dev->fw_ver[7], 0);
		file_fw_ver =
			U82U64(set->fw_ver[0], 7) + U82U64(set->fw_ver[1], 6) +
			U82U64(set->fw_ver[2], 5) + U82U64(set->fw_ver[3], 4) +
			U82U64(set->fw_ver[4], 3) + U82U64(set->fw_ver[5], 2) +
			U82U64(set->fw_ver[6], 1) + U82U64(set->fw_ver[7], 0);

		TP_MSG(dev->id, "IC fw ver: 0x" PFMT_X64 ", File fw ver: 0x" PFMT_X64 "\n",
			(long long unsigned int)dev_fw_ver,
			(long long unsigned int)file_fw_ver);

		if (file_fw_ver > dev_fw_ver) {
			TP_INFO(dev->id, "IC FW version is older than File FW version\n");
			return true;
		} else if (file_fw_ver == dev_fw_ver) {
			TP_INFO(dev->id, "File FW version is the same, " PFMT_C8 " to update\n",
				(set->fw_ver_policy & allow_fw_ver_same) ?
				"still need" : "no need");
			return (set->fw_ver_policy & allow_fw_ver_same) ?
				need : false;
		} else {
			TP_INFO(dev->id, "File FW version is older, " PFMT_C8 " to update\n",
				(set->fw_ver_policy & allow_fw_ver_downgrade) ?
				"still need" : "no need");
			return (set->fw_ver_policy & allow_fw_ver_downgrade) ?
				need : false;
		}
	}

	return need;
}

static int update_master(struct ilitek_fw_handle *fw, int idx, u32 len)
{
	int error = 0;
	struct ilitek_ts_device *dev = fw->dev;
	unsigned int i;
	u16 file_crc;
	int retry = 3;

	TP_MSG(dev->id, "updating block[%d], data len: %u, start/end addr: 0x%x/0x%x\n",
		idx, len, fw->file.blocks[idx].start, fw->file.blocks[idx].end);

err_retry:
	if ((dev->setting.no_retry && error < 0) || retry-- < 0)
		return (error < 0) ? error : -EINVAL;

	error = api_write_enable_v6(dev, false, false,
				     fw->file.blocks[idx].start,
				     fw->file.blocks[idx].end);
	if (error < 0)
		return error;

	_memset(dev->wbuf, 0xff, sizeof(dev->wbuf));
	for (i = fw->file.blocks[idx].start;
	     i < fw->file.blocks[idx].end; i += len) {
		/*
		 * check end addr. of data write buffer is within valid range.
		 */
		if (i + len > END_ADDR_LEGO) {
			TP_ERR(dev->id, "block[%d] write addr. 0x%x + 0x%x > 0x%x OOB\n",
				idx, i, len, END_ADDR_LEGO);
			return -EINVAL;
		}

		_memcpy(dev->wbuf + 1, fw->file.buf + i, len);
		error = api_write_data_v6(dev, len + 1);

		if (error < 0)
			goto err_retry;

		fw->progress_curr = MIN(i + len - fw->file.blocks[idx].offset,
					fw->progress_max);
		fw->progress = (100 * fw->progress_curr) / fw->progress_max;
		TP_DBG(dev->id, "block[%d] update progress: " PFMT_U8 "%%\n",
			idx, fw->progress);

		if (fw->cb.update_progress)
			fw->cb.update_progress(fw->progress, fw->_private);
	}

	file_crc = get_crc(fw->file.blocks[idx].start,
			   fw->file.blocks[idx].end - 1,
			   fw->file.buf, fw->file.buf_size);
	dev->ic[0].crc[idx] =
		api_get_block_crc_by_addr(dev, CRC_GET,
					  fw->file.blocks[idx].start,
					  fw->file.blocks[idx].end);

	TP_INFO(dev->id, "block[%d]: start/end addr.: 0x%x/0x%x, ic/file crc: 0x%x/0x%x " PFMT_C8 "\n",
		idx, fw->file.blocks[idx].start, fw->file.blocks[idx].end,
		dev->ic[0].crc[idx], file_crc,
		(file_crc == dev->ic[0].crc[idx]) ?
		"matched" : "not matched");

	if (file_crc != dev->ic[0].crc[idx]) {
		error = -EINVAL;
		goto err_retry;
	}

	return 0;
}

static int update_slave(struct ilitek_fw_handle *fw)
{
	int error;
	struct ilitek_ts_device *dev = fw->dev;
	u8 i;

	for (i = 0; i < fw->file.block_num; i++) {
		dev->ic[0].crc[i] = api_get_block_crc_by_addr(dev,
			CRC_CALCULATE, fw->file.blocks[i].start,
			fw->file.blocks[i].end);
	}

	error = api_protocol_set_cmd(dev, GET_AP_CRC, &dev->tp_info.ic_num);
	if (error < 0)
		return error;

	for (i = 0; i < dev->tp_info.ic_num; i++) {
		if (dev->ic[0].crc[0] == dev->ic[i].crc[0] &&
		    !fw->setting.force_update)
			continue;

		TP_INFO(dev->id, "updating slave, master/slave[" PFMT_U8 "] crc: 0x%x/0x%x\n",
			i, dev->ic[0].crc[0], dev->ic[i].crc[0]);
		error = api_access_slave(dev, 0x3, CMD_WRITE_DATA_V6, NULL);
		if (error < 0)
			return error;
		error = api_write_enable_v6(dev, false, true,
					    fw->file.blocks[0].start,
					    fw->file.blocks[0].end);
		if (error < 0)
			return error;

		goto success_return;
	}

	error = api_protocol_set_cmd(dev, GET_MCU_MOD, &dev->tp_info.ic_num);
	if (error < 0)
		return error;

	for (i = 0; i < dev->tp_info.ic_num; i++) {
		if (dev->ic[i].mode == AP_MODE &&
		    !fw->setting.force_update)
			continue;

		TP_INFO(dev->id, "changing slave[" PFMT_U8 "]: 0x" PFMT_X8 " to AP mode\n",
			i, dev->ic[i].mode);
		error = api_access_slave(dev, 0x3, CMD_SET_AP_MODE, NULL);
		if (error < 0)
			return error;
		break;
	}

success_return:
	return 0;
}

static int update_M3_M2V(struct ilitek_fw_handle *fw, u32 len)
{
	int error;
	struct ilitek_ts_device *dev = fw->dev;
	u32 i;
	u8 buf[6];
	u32 AdressRange, M2V_Buf_Len, CheckSumRange;
	u16 AddDataCount;

	u8 j = fw->file.block_num;

	error = api_set_ctrl_mode(dev, mode_suspend, false, true);
	if (error < 0)
		return error;

	api_protocol_set_cmd(dev, GET_PTL_VER, NULL);
	api_protocol_set_cmd(dev, GET_PTL_VER, NULL);

	//Reed Add : 20230721
	AdressRange = fw->file.blocks[j].end - fw->file.blocks[j].start;
	M2V_Buf_Len = fw->file.blocks[j].end - fw->file.blocks[j].start + 1;
	CheckSumRange = fw->file.blocks[j].check;
	AddDataCount = len - M2V_Buf_Len % len;
	AddDataCount = (AddDataCount >= 8) ? AddDataCount : AddDataCount + len;
	AdressRange += AddDataCount;
	CheckSumRange += AddDataCount * 0xFF;

	error = api_to_bl_mode_m2v(dev, true);
	if (error < 0)
		return error;

	dev->cb.delay_ms(100);//Reed Add : 20230927

	error = api_set_data_len(dev, len);
	if (error < 0)
		return error;

	TP_INFO(dev->id, "updating M2V, start/end addr.: 0x%x/0x%x, file checksum: 0x%x\n",
		fw->file.blocks[j].start, fw->file.blocks[j].end,
		fw->file.blocks[j].check);

	//Reed Add : 20230721
	buf[0] = (AdressRange >> 16) & 0xFF;
	buf[1] = (AdressRange >> 8) & 0xFF;
	buf[2] = AdressRange & 0xFF;
	buf[3] = (CheckSumRange >> 16) & 0xFF;
	buf[4] = (CheckSumRange >> 8) & 0xFF;
	buf[5] = CheckSumRange & 0xFF;
	dev->cb.delay_ms(100);//Reed Add : 20230927
	error = api_access_slave(dev, 0x80, CMD_WRITE_ENABLE, buf);
	if (error < 0)
		return error;

	_memset(dev->wbuf, 0xff, sizeof(dev->wbuf));
	for (i = fw->file.blocks[j].start;
	     i < fw->file.blocks[j].end; i += len) {
		_memcpy(dev->wbuf + 1, fw->m2v_buf + i, len);

		error = api_write_data_m2v(dev, len + 1);
		if (error < 0)
			return error;

		fw->progress_curr = MIN(fw->progress_curr + len,
					fw->progress_max);
		fw->progress = (100 * fw->progress_curr) / fw->progress_max;
		TP_DBG(dev->id, "m2v update progress: " PFMT_U8 "%%\n", fw->progress);

		if (fw->cb.update_progress)
			fw->cb.update_progress(fw->progress, fw->_private);
	}

	error = api_to_bl_mode_m2v(dev, false);
	if (error < 0)
		return error;

	dev->cb.delay_ms(100);//Reed Add : 20230927

	error = api_access_slave(dev, 0x80, CMD_GET_FW_VER, fw->m2v_fw_ver);
	if (error < 0)
		return error;

	TP_MSG_ARR(dev->id, "update M2V success, fw version:", TYPE_U8,
		   8, fw->m2v_fw_ver);

	return 0;
}

static int ilitek_update_BL_v1_8(struct ilitek_fw_handle *fw)
{
	int error;
	struct ilitek_ts_device *dev = fw->dev;
	u8 i;

	error = api_set_data_len(dev, fw->update_len);
	if (error < 0)
		return error;

	for (i = 0; i < fw->file.block_num; i++) {
		if (fw->file.blocks[i].check_match)
			continue;

		error = update_master(fw, i, fw->update_len);
		if (error < 0) {
			TP_ERR(dev->id, "Upgrade Block:" PFMT_U8 " failed, err: %d\n",
				i, error);
			return error;
		}
	}

	error = api_to_bl_mode(dev, false, fw->file.blocks[0].start,
			       fw->file.blocks[0].end);
	if (error < 0)
		return error;

	error = api_set_ctrl_mode(dev, mode_suspend, false, true);
	if (error < 0)
		return error;

	/* get tp info. for updating ic_num */
	error = api_protocol_set_cmd(dev, GET_TP_INFO, NULL);
	if (error < 0)
		return error;

	if (dev->tp_info.ic_num > 1) {
		if (fw->cb.slave_update_notify)
			fw->cb.slave_update_notify(true, fw->_private);
		error = update_slave(fw);

		if (fw->cb.slave_update_notify)
			fw->cb.slave_update_notify(false, fw->_private);

		if (error < 0) {
			TP_ERR(dev->id, "upgrade slave failed, err: %d\n",
				error);
			return error;
		}
	}

	if (fw->m2v && fw->m2v_need_update) {
		error = update_M3_M2V(fw, 1024);
		if (error < 0) {
			TP_ERR(dev->id, "upgrade m2v slave failed, err: %d\n", error);
			return error;
		}
	}

	return 0;
}

static int ilitek_update_BL_v1_7(struct ilitek_fw_handle *fw)
{
	int error;
	struct ilitek_ts_device *dev = fw->dev;
	unsigned int i;

	/*
	 * Erase data initially for the case that hex w/o data flash section.
	 * Due to historical factor, please double confirm before modify here.
	 */
	error = api_erase_data_v3(dev);
	if (error < 0)
		return error;

	if (fw->file.blocks[1].end > fw->file.blocks[1].start) {
		TP_MSG(dev->id, "updating DF block, start/end addr.: 0x%x/0x%x, file checksum: 0x%x\n",
			fw->file.blocks[1].start, fw->file.blocks[1].end,
			fw->file.blocks[1].check);

		/* end + 1 as W.A. for V3 BL bug */
		error = api_write_enable_v3(dev, false, false,
					     fw->file.blocks[1].end + 1,
					     fw->file.blocks[1].check);
		if (error < 0)
			return error;

		for (i = fw->file.blocks[1].start;
		     i <= fw->file.blocks[1].end; i += 32) {
			_memset(dev->wbuf + 1, 0, 32);
			_memcpy(dev->wbuf + 1, fw->file.buf + i,
				MIN(fw->file.blocks[1].end - i + 1, 32));

			error = api_write_data_v3(dev);
			if (error < 0)
				return error;

			dev->cb.delay_ms(2);

			error = api_check_busy(dev, 1000, 10);
			if (error < 0)
				return error;

			fw->progress_curr += 32;
			fw->progress_curr = MIN(fw->progress_curr,
				fw->progress_max);
			fw->progress = (100 * fw->progress_curr) /
				fw->progress_max;
			TP_DBG(dev->id, "DF update progress: " PFMT_U8 "%%\n",
				fw->progress);

			if (fw->cb.update_progress)
				fw->cb.update_progress(fw->progress,
						       fw->_private);
		}

		dev->cb.delay_ms(50);

		dev->wbuf[0] = CMD_GET_AP_CRC;
		error = write_then_read(dev, dev->wbuf, 1, NULL, 0);
		if (error < 0)
			return error;
		error = api_check_busy(dev, 1000, 10);
		if (error < 0)
			return error;

		dev->wbuf[0] = CMD_GET_AP_CRC;
		error = write_then_read(dev, dev->wbuf, 1, dev->rbuf, 4);
		if (error < 0)
			return error;

		dev->ic[0].crc[1] = (le16(dev->rbuf + 2) << 16) | le16(dev->rbuf);

		TP_INFO(dev->id, "DF block, start/end addr.: 0x%x/0x%x, ic/file checkksum: 0x%x/0x%x " PFMT_C8 "\n",
			fw->file.blocks[1].start, fw->file.blocks[1].end,
			dev->ic[0].crc[1], fw->file.blocks[1].check,
			(dev->ic[0].crc[1] == fw->file.blocks[1].check) ?
			"matched" : "not matched");

		if (dev->ic[0].crc[1] != fw->file.blocks[1].check)
			return -EFAULT;
	}

	/*
	 * Update AP code forcely if AP crc/checksum not match or
	 * Data Flash has been updated.
	 */
	if (!fw->file.blocks[0].check_match ||
	    fw->file.blocks[1].end > fw->file.blocks[1].start) {
		TP_MSG(dev->id, "updating AP block, start/end addr.: 0x%x/0x%x, file crc: 0x%x\n",
			fw->file.blocks[0].start, fw->file.blocks[0].end,
			fw->file.blocks[0].check);

		/* end + 1 as W.A. for V3 BL bug */
		error = api_write_enable_v3(dev, false, true,
					     fw->file.blocks[0].end + 1,
					     fw->file.blocks[0].check);
		if (error < 0)
			return error;

		for (i = fw->file.blocks[0].start;
		     i <= fw->file.blocks[0].end; i += 32) {
			_memset(dev->wbuf + 1, 0xFF, 32);
			_memcpy(dev->wbuf + 1, fw->file.buf + i,
				MIN(fw->file.blocks[1].end - i + 1, 32));
			error = api_write_data_v3(dev);
			if (error < 0)
				return error;

			dev->cb.delay_ms(2);

			error = api_check_busy(dev, 1000, 10);
			if (error < 0)
				return error;

			fw->progress_curr += 32;
			fw->progress_curr = MIN(fw->progress_curr,
				fw->progress_max);
			fw->progress = (100 * fw->progress_curr) /
				fw->progress_max;
			TP_DBG(dev->id, "AP update progress: " PFMT_U8 "%%\n",
				fw->progress);

			if (fw->cb.update_progress)
				fw->cb.update_progress(fw->progress,
						       fw->_private);
		}

		dev->wbuf[0] = CMD_GET_AP_CRC;
		error = write_then_read(dev, dev->wbuf, 1, NULL, 0);
		if (error < 0)
			return error;
		error = api_check_busy(dev, 1000, 10);
		if (error < 0)
			return error;

		dev->wbuf[0] = CMD_GET_AP_CRC;
		error = write_then_read(dev, dev->wbuf, 1, dev->rbuf, 4);
		if (error < 0)
			return error;

		dev->ic[0].crc[0] =
			(le16(dev->rbuf + 2) << 16) | le16(dev->rbuf);
		TP_INFO(dev->id, "AP block, start/end addr.: 0x%x/0x%x, ic/file crc: 0x%x/0x%x " PFMT_C8 "\n",
			fw->file.blocks[0].start, fw->file.blocks[0].end,
			dev->ic[0].crc[0], fw->file.blocks[0].check,
			(dev->ic[0].crc[0] == fw->file.blocks[0].check) ?
			"matched" : "not matched");

		if (dev->ic[0].crc[0] != fw->file.blocks[0].check)
			return -EFAULT;
	}

	return 0;
}

static int ilitek_update_BL_v1_6(struct ilitek_fw_handle *fw)
{
	int error;
	struct ilitek_ts_device *dev = fw->dev;
	struct ilitek_ts_callback *cb = &dev->cb;
	unsigned int i, j;

	u32 bytes, end, check;

	if (fw->file.blocks[1].end > fw->file.blocks[1].start) {
		TP_INFO(dev->id, "updating DF block, start/end addr.: 0x%x/0x%x, file checksum: 0x%x\n",
			fw->file.blocks[1].start, fw->file.blocks[1].end,
			fw->file.blocks[1].check);

		/* BL 1.6 need more 32 byte 0xFF, end addr need to be multiples of 32 */
		bytes = ((fw->file.blocks[1].end + 1) % 32) ?
			31 + 32 - ((fw->file.blocks[1].end + 1) % 32) : 31;
		end = fw->file.blocks[1].end + bytes;
		check = fw->file.blocks[1].check + (bytes * 0xff);
		TP_MSG(dev->id, "(after modified) DF start/end: 0x%x/0x%x, checksum: 0x%x\n",
			fw->file.blocks[1].start, end, check);

		/* end + 1 as W.A. for V3 BL bug */
		error = api_write_enable_v3(dev, false, false, end + 1, check);
		if (error < 0)
			return error;

		for (i = fw->file.blocks[1].start, j = 0;
		     i <= fw->file.blocks[1].end; i += 32, j++) {
			_memset(dev->wbuf + 1, 0xFF, 32);
			_memcpy(dev->wbuf + 1, fw->file.buf + i,
				MIN(fw->file.blocks[1].end - i + 1, 32));
			error = api_write_data_v3(dev);
			if (error < 0)
				return error;

			cb->delay_ms((j % 16) ? 1 : 5);

			fw->progress_curr = MIN(fw->progress_curr + 32,
				fw->progress_max);
			fw->progress = (100 * fw->progress_curr) / fw->progress_max;
			TP_DBG(dev->id, "DF update progress: " PFMT_U8 "%%\n", fw->progress);

			if (fw->cb.update_progress)
				fw->cb.update_progress(fw->progress, fw->_private);
		}
		cb->delay_ms(10);
		/* write 31 bytes 0xFF at the end */
		_memset(dev->wbuf + 1, 0xFF, 32); dev->wbuf[32] = 0;
		error = api_write_data_v3(dev);
		if (error < 0)
			return error;
		cb->delay_ms(10);
	}

	/*
	* Update AP code forcely if AP crc/checksum not match or
	* Data Flash has been updated.
	*/
	if (!fw->file.blocks[0].check_match ||
	    fw->file.blocks[1].end > fw->file.blocks[1].start) {
		TP_INFO(dev->id, "updating AP block, start/end addr.: 0x%x/0x%x, file crc: 0x%x\n",
			fw->file.blocks[0].start, fw->file.blocks[0].end,
			fw->file.blocks[0].check);

		/* BL 1.6 need more 32 byte 0xFF, end addr need to be multiples of 32 */
		bytes = ((fw->file.blocks[0].end + 1) % 32) ?
			31 + 32 - ((fw->file.blocks[0].end + 1) % 32) : 31;
		end = fw->file.blocks[0].end + bytes;
		check = fw->file.blocks[0].check + (bytes * 0xff);
		TP_MSG(dev->id, "(after modified) AP start/end: 0x%x/0x%x, checksum: 0x%x\n",
			fw->file.blocks[0].start, end, check);

		/* end + 1 as W.A. for V3 BL bug */
		error = api_write_enable_v3(dev, false, true, end + 1, check);
		if (error < 0)
			return error;

		for (i = fw->file.blocks[0].start, j = 0;
			i <= fw->file.blocks[0].end; i += 32, j++) {
			_memset(dev->wbuf + 1, 0xFF, 32);
			_memcpy(dev->wbuf + 1, fw->file.buf + i,
				MIN(fw->file.blocks[0].end - i + 1, 32));

			error = api_write_data_v3(dev);
			if (error < 0)
				return error;

			cb->delay_ms((j % 16) ? 1 : 5);

			fw->progress_curr = MIN(fw->progress_curr + 32,
				fw->progress_max);
			fw->progress = (100 * fw->progress_curr) / fw->progress_max;
			TP_DBG(dev->id, "AP update progress: " PFMT_U8 "%%\n", fw->progress);

			if (fw->cb.update_progress)
				fw->cb.update_progress(fw->progress, fw->_private);
		}
		cb->delay_ms(10);
		/* write 31 bytes 0xFF at the end */
		_memset(dev->wbuf + 1, 0xFF, 32); dev->wbuf[32] = 0;
		error = api_write_data_v3(dev);
		if (error < 0)
			return error;
		cb->delay_ms(10);

#ifdef ILITEK_BOOT_UPDATE
		/*
		 * .ili file fill DF section with default 0xFF, but not 0
		 * which make flow stuck in BL mode.
		 * so HW-reset is need before switching to AP mode.
		 * remove HW-reset after fixing .ili converter bug.
		 */
		reset_helper(dev);
#endif
	}

	return 0;
}

static void update_progress(struct ilitek_fw_handle *fw)
{
	struct ilitek_ts_device *dev = fw->dev;
	u8 i;
	unsigned int last_end = 0, last_offset = 0;

	fw->progress = 0;
	fw->progress_max = 0;
	fw->progress_curr = 0;

	switch (dev->protocol.flag) {
	case PTL_V3:
		fw->progress_max +=
			(fw->file.blocks[1].end > fw->file.blocks[1].start) ?
			fw->file.blocks[1].end - fw->file.blocks[1].start : 0;
		fw->progress_max +=
			(fw->file.blocks[0].end > fw->file.blocks[0].start) ?
			fw->file.blocks[0].end - fw->file.blocks[0].start : 0;
		break;

	case PTL_V6:
		for (i = 0; i < fw->file.block_num; i++) {
			if (fw->file.blocks[i].check_match)
				continue;

			fw->progress_max +=
				fw->file.blocks[i].end -
				fw->file.blocks[i].start;
			last_offset += fw->file.blocks[i].start - last_end;
			fw->file.blocks[i].offset = last_offset;

			last_end = fw->file.blocks[i].end;
		}

		if (fw->m2v_need_update) {
			fw->progress_max +=
				fw->file.blocks[fw->file.block_num].end -
				fw->file.blocks[fw->file.block_num].start;
		}

		break;
	}
}

void *ilitek_update_init(void *_dev, bool need_update_ts_info,
			 struct ilitek_update_callback *cb, void *_private)
{
	struct ilitek_fw_handle *fw;
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)_dev;

	if (need_update_ts_info && dev && api_update_ts_info(dev) < 0)
		return NULL;

	fw = (struct ilitek_fw_handle *)MALLOC(sizeof(*fw));
	if (!fw)
		return NULL;

	/* initial all member to 0/ false/ NULL */
	_memset(fw, 0, sizeof(*fw));
	fw->dev = (dev) ? dev : NULL;

	/* initial update-len to default UPDATE_LEN */
	fw->update_len = UPDATE_LEN;

	fw->dev = dev;
	fw->_private = _private;
	fw->file.buf_size = ILITEK_FW_BUF_SIZE;
	fw->file.buf = (u8 *)CALLOC(fw->file.buf_size, 1);
	if (!fw->file.buf)
		goto err_free_fw;

	fw->m2v_buf = (u8 *)CALLOC(ILITEK_FW_BUF_SIZE, 1);
	if (!fw->m2v_buf)
		goto err_free_fw_buf;

	if (cb)
		_memcpy(&fw->cb, cb, sizeof(*cb));

	return fw;

err_free_fw_buf:
	CFREE(fw->file.buf);
err_free_fw:
	FREE(fw);

	return NULL;
}

void ilitek_update_exit(void *handle)
{
	struct ilitek_fw_handle *fw = (struct ilitek_fw_handle *)handle;

	if (!handle)
		return;

	if (fw->file.buf)
		CFREE(fw->file.buf);

	if (fw->m2v_buf)
		CFREE(fw->m2v_buf);

	if (fw)
		FREE(fw);
}

void ilitek_update_set_data_length(void *handle, u16 len)
{
	struct ilitek_fw_handle *fw = (struct ilitek_fw_handle *)handle;

	if (!fw)
		return;

	fw->update_len = len;
}

int ilitek_update_load_fw(void *handle, WCHAR *fw_name)
{
	int error;
	struct ilitek_fw_handle *fw = (struct ilitek_fw_handle *)handle;

	u32 i;

	if (!handle)
		return -EINVAL;

	error = decode_firmware(fw, fw_name);
	if (error < 0)
		return error;

	if (fw->cb.update_fw_file_info)
		fw->cb.update_fw_file_info(&fw->file, fw->_private);

	if (!fw->dev)
		return 0;

	/* for Lego and V6 IC, check block's start/end address validity */
	if (fw->dev->protocol.flag == PTL_V6) {
		for (i = 0; i < fw->file.block_num; i++) {
			if (fw->dev->mcu_info.min_addr <=
				fw->file.blocks[i].start &&
			    fw->dev->mcu_info.max_addr >
				fw->file.blocks[i].end)
				continue;

			if (!(fw->file.blocks[i].start % 0x1000))
				continue;

			TP_ERR(fw->dev->id, "Block[%u] addr. OOB (0x%x <= 0x%x/0x%x < 0x%x) or invalid start addr\n",
				i, fw->dev->mcu_info.min_addr,
				fw->file.blocks[i].start,
				fw->file.blocks[i].end,
				fw->dev->mcu_info.max_addr);
			return -EINVAL;
		}
	}

	TP_MSG(fw->dev->id, "IC: " PFMT_C8 ", Firmware File: " PFMT_C8 " matched\n",
		fw->dev->mcu_info.ic_name, fw->file.ic_name);

	return 0;
}

int ilitek_update_start(void *handle)
{
	int error;
	s8 retry = 0;
	struct ilitek_fw_handle *fw = (struct ilitek_fw_handle *)handle;
	struct ilitek_ts_device *dev;

	if (!handle)
		return -EINVAL;
	dev = fw->dev;

	/*
	 * Some platform (ITS-Bridge) might change touch controller
	 * after loading fw file, get panel info. forcely and
	 * re-check the ic/file are matched.
	 */
	error = api_update_ts_info(dev);
	if (error < 0 || strcmp(dev->mcu_info.ic_name, fw->file.ic_name)) {
		TP_ERR(fw->dev->id, "get ic info failed, err: %d or ic/file (" PFMT_C8 "/" PFMT_C8 ") not matched\n",
			error, fw->dev->mcu_info.ic_name, fw->file.ic_name);
		return -EPERM;
	}

	TP_INFO(dev->id, "[ilitek_update_start] start\n");

	do {
		TP_DBG(dev->id, "retry: %hhd, retry_limit: %hhd\n",
			retry, fw->setting.retry);
		if (retry)
			reset_helper(dev);

		error = api_set_ctrl_mode(dev, mode_suspend, false, true);
		if (error < 0)
			continue;

		if (!need_fw_update(fw)) {
			if (is_231x(dev))
				goto erase_data_flash_231x;
			goto success_return;
		}

		update_progress(fw);
		if (fw->cb.update_progress)
			fw->cb.update_progress(0, fw->_private);

		error = api_to_bl_mode(dev, true, 0, 0);
		if (error < 0)
			continue;

		TP_INFO_ARR(dev->id, "[BL Firmware Version]",
			    TYPE_U8, 8, dev->fw_ver);
		TP_INFO(dev->id, "[ilitek_update_start] start to program\n");

		switch (dev->protocol.ver & 0xFFFF00) {
		case BL_PROTOCOL_V1_8:
			error = ilitek_update_BL_v1_8(fw);
			break;
		case BL_PROTOCOL_V1_7:
			error = ilitek_update_BL_v1_7(fw);
			break;
		case BL_PROTOCOL_V1_6:
			error = ilitek_update_BL_v1_6(fw);
			break;
		default:
			TP_ERR(dev->id, "BL protocol ver: 0x%x not supported\n",
				dev->protocol.ver);
			continue;
		}
		if (error < 0)
			continue;

		error = api_to_bl_mode(dev, false,
				       fw->file.blocks[0].start,
				       fw->file.blocks[0].end);
		if (error < 0)
			continue;

erase_data_flash_231x:
		/*
		 * If no data flash section in firmware file,
		 * 231x need to erase data flash after change to AP mode.
		 */
		if (fw->file.blocks[1].end < fw->file.blocks[1].start &&
		    is_231x(dev)) {
			error = api_erase_data_v3(dev);
			if (error < 0)
				continue;
		}

success_return:
		error = api_update_ts_info(dev);
		if (error < 0)
			continue;

		error = api_set_ctrl_mode(dev, mode_normal, false, true);
		if (error < 0)
			continue;

		if (fw->cb.update_fw_ic_info)
			fw->cb.update_fw_ic_info(true,
						 dev->fw_ver, dev->ic[0].crc,
						 fw->file.block_num,
						 fw->_private);

		if (fw->cb.update_progress)
			fw->cb.update_progress(100, fw->_private);

		if (need_retry(fw))
			continue;

		TP_INFO(dev->id, "[ilitek_update_start] success\n");

		return 0;
	} while (!dev->setting.no_retry && ++retry < fw->setting.retry);

	TP_ERR(dev->id, "[ilitek_update_start] fw update failed, err: %d\n",
		error);

	return (error < 0) ? error : -EFAULT;
}

void ilitek_update_setting(void *handle, struct ilitek_fw_settings *setting)
{
	struct ilitek_fw_handle *fw = (struct ilitek_fw_handle *)handle;

	if (!handle)
		return;

	_memcpy(&fw->setting, setting, sizeof(struct ilitek_fw_settings));
}

