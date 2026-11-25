// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright (c) 2024 Imagination Technologies Ltd. */

#include "pvr_device.h"
#include "pvr_fw.h"

#include <drm/drm_device.h>
#include <drm/drm_print.h>

#include <linux/elf.h>
#include <linux/string.h>
#include <linux/types.h>

/**
 * pvr_fw_process_elf_command_stream() - Process ELF firmware image and populate
 *                                       firmware sections
 * @pvr_dev: Device pointer.
 * @fw: Pointer to firmware image.
 * @segments: Firmware segments, includes host pointers for populating and
 * firmware objects for reference.
 *
 * Returns:
 *  * 0 on success, or
 *  * -%EINVAL on any error in ELF command stream.
 */
int
pvr_fw_process_elf_command_stream(struct pvr_device *pvr_dev, const u8 *fw,
				  const struct pvr_fw_segment segments[static PVR_FW_SEGMENT_COUNT])
{
	struct elf32_hdr *header = (struct elf32_hdr *)fw;
	struct elf32_phdr *program_header = (struct elf32_phdr *)(fw + header->e_phoff);
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	int err;

	for (u32 entry = 0; entry < header->e_phnum; entry++, program_header++) {
		void *write_addr;

		/* Only consider loadable entries in the ELF segment table */
		if (program_header->p_type != PT_LOAD)
			continue;

		err = pvr_fw_find_mmu_segment(pvr_dev, program_header->p_vaddr,
					      program_header->p_memsz, segments,
					      &write_addr);
		if (err) {
			drm_err(drm_dev,
				"Addr 0x%x (size: %d) not found in any firmware segment",
				program_header->p_vaddr, program_header->p_memsz);
			return err;
		}

		/* Write to FW allocation only if available */
		if (write_addr) {
			memcpy(write_addr, fw + program_header->p_offset,
			       program_header->p_filesz);

			memset((u8 *)write_addr + program_header->p_filesz, 0,
			       program_header->p_memsz - program_header->p_filesz);
		}
	}

	return 0;
}
