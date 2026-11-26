// SPDX-License-Identifier: GPL-2.0

#include <linux/efi.h>
#include <linux/zstd.h>

#include <asm/efi.h>

#include "decompress_sources.h"
#include "efistub.h"

extern unsigned char _gzdata_start[], _gzdata_end[];

static size_t wksp_size;
static void *wksp;

efi_status_t efi_zboot_decompress_init(unsigned long *alloc_size)
{
	zstd_frame_header header;
	efi_status_t status;
	size_t ret;

	ret = zstd_get_frame_header(&header, _gzdata_start,
				    _gzdata_end - _gzdata_start);
	if (ret != 0 ||
	    header.frameContentSize == 0 ||
	    header.frameContentSize == ZSTD_CONTENTSIZE_UNKNOWN ||
	    header.frameContentSize != (unsigned long)header.frameContentSize) {
		efi_err("ZSTD-compressed data has an invalid frame header\n");
		return EFI_LOAD_ERROR;
	}

	wksp_size = zstd_dctx_workspace_bound();
	status = efi_allocate_pages(wksp_size, (unsigned long *)&wksp, ULONG_MAX);
	if (status != EFI_SUCCESS)
		return status;

	*alloc_size = header.frameContentSize;
	return EFI_SUCCESS;
}

efi_status_t efi_zboot_decompress(u8 *out, unsigned long outlen)
{
	zstd_dctx *dctx = zstd_init_dctx(wksp, wksp_size);
	size_t ret;
	int retval;

	ret = zstd_decompress_dctx(dctx, out, outlen, _gzdata_start,
				   _gzdata_end - _gzdata_start);
	efi_free(wksp_size, (unsigned long)wksp);

	retval = zstd_get_error_code(ret);
	if (retval) {
		efi_err("ZSTD-decompression failed with status %d\n", retval);
		return EFI_LOAD_ERROR;
	}

	return EFI_SUCCESS;
}
