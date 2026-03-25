// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES.  All rights reserved.
 */
#include <stdint.h>
#include <strings.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#include <linux/errno.h>
#include <linux/io.h>
#include <linux/pci_ids.h>

#include <libvfio.h>

#include "hw.h"

static inline struct gpu_device *to_nv_gpu(struct vfio_pci_device *device)
{
	return device->driver.region.vaddr;
}

static enum gpu_arch nv_gpu_arch_lookup(u32 pmc_boot_0)
{
	u32 arch = (pmc_boot_0 >> 24) & 0x1f;

	switch (arch) {
	case 0x0e:
	case 0x0f:
	case 0x10:
		return GPU_ARCH_KEPLER;
	case 0x11:
		return GPU_ARCH_MAXWELL_GEN1;
	case 0x12:
		return GPU_ARCH_MAXWELL_GEN2;
	case 0x13:
		/* P100 (impl 0) uses PMC reset; P4/P40 use engine reset */
		if (((pmc_boot_0 >> 20) & 0xf) == 0)
			return GPU_ARCH_PASCAL;
		return GPU_ARCH_PASCAL_10X;
	case 0x14:
		return GPU_ARCH_VOLTA;
	case 0x16:
		return GPU_ARCH_TURING;
	case 0x17:
		return GPU_ARCH_AMPERE;
	case 0x18:
		return GPU_ARCH_HOPPER;
	case 0x19:
		return GPU_ARCH_ADA;
	default:
		return GPU_ARCH_UNKNOWN;
	}
}

static inline u32 gpu_read32(struct gpu_device *gpu, u32 offset)
{
	return readl(gpu->bar0 + offset);
}

static inline void gpu_write32(struct gpu_device *gpu, u32 offset, u32 value)
{
	writel(value, gpu->bar0 + offset);
}

static int gpu_poll_register(struct vfio_pci_device *device,
			     const char *name, u32 offset,
			     u32 expected, u32 mask, u32 timeout_ms)
{
	struct gpu_device *gpu = to_nv_gpu(device);
	u32 value;
	struct timespec start, now;
	u64 elapsed_ms;

	clock_gettime(CLOCK_MONOTONIC, &start);

	for (;;) {
		value = gpu_read32(gpu, offset);
		if ((value & mask) == expected)
			return 0;

		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed_ms = (now.tv_sec - start.tv_sec) * 1000
			     + (now.tv_nsec - start.tv_nsec) / 1000000;

		if (elapsed_ms >= timeout_ms)
			break;

		usleep(1000);
	}

	dev_err(device,
		"Timeout polling %s (0x%x): value=0x%x expected=0x%x mask=0x%x after %llu ms\n",
		name, offset, value, expected, mask,
		(unsigned long long)elapsed_ms);
	return -ETIMEDOUT;
}

static int fsp_poll_queue(struct gpu_device *gpu, u32 head_reg, u32 tail_reg,
			  bool wait_empty, u32 timeout_ms)
{
	struct timespec start, now;
	u64 elapsed_ms;
	u32 head, tail;

	clock_gettime(CLOCK_MONOTONIC, &start);

	for (;;) {
		head = gpu_read32(gpu, head_reg);
		tail = gpu_read32(gpu, tail_reg);
		if (wait_empty ? (head == tail) : (head != tail))
			return 0;

		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed_ms = (now.tv_sec - start.tv_sec) * 1000
			     + (now.tv_nsec - start.tv_nsec) / 1000000;

		if (elapsed_ms >= timeout_ms)
			return -ETIMEDOUT;

		usleep(1000);
	}
}

static void fsp_emem_write(struct gpu_device *gpu, u32 offset,
			   const u32 *data, u32 count)
{
	u32 i;

	/* Configure port with auto-increment for read and write */
	gpu_write32(gpu, NV_FSP_EMEM_PORT2_CTRL,
		    offset | NV_FALCON_EMEMC_AINCR | NV_FALCON_EMEMC_AINCW);

	for (i = 0; i < count; i++)
		gpu_write32(gpu, NV_FSP_EMEM_PORT2_DATA, data[i]);
}

static void fsp_emem_read(struct gpu_device *gpu, u32 offset,
			  u32 *data, u32 count)
{
	u32 i;

	/* Configure port with auto-increment for read and write */
	gpu_write32(gpu, NV_FSP_EMEM_PORT2_CTRL,
		    offset | NV_FALCON_EMEMC_AINCR | NV_FALCON_EMEMC_AINCW);

	for (i = 0; i < count; i++)
		data[i] = gpu_read32(gpu, NV_FSP_EMEM_PORT2_DATA);
}

static int fsp_rpc_send_data(struct gpu_device *gpu, const u32 *data, u32 count)
{
	int ret;

	ret = fsp_poll_queue(gpu, NV_FSP_QUEUE_HEAD, NV_FSP_QUEUE_TAIL, true, 1000);
	if (ret)
		return ret;

	fsp_emem_write(gpu, NV_FSP_RPC_EMEM_BASE, data, count);

	/* Update queue head/tail to signal data is ready */
	gpu_write32(gpu, NV_FSP_QUEUE_TAIL,
		    NV_FSP_RPC_EMEM_BASE + (count - 1) * 4);
	gpu_write32(gpu, NV_FSP_QUEUE_HEAD, NV_FSP_RPC_EMEM_BASE);

	return 0;
}

static int fsp_rpc_receive_data(struct gpu_device *gpu, u32 *data,
				u32 max_count, u32 timeout_ms)
{
	u32 head, tail;
	u32 msg_size_words;
	int ret;

	ret = fsp_poll_queue(gpu, NV_FSP_MSG_QUEUE_HEAD, NV_FSP_MSG_QUEUE_TAIL,
			     false, timeout_ms);
	if (ret)
		return ret;

	head = gpu_read32(gpu, NV_FSP_MSG_QUEUE_HEAD);
	tail = gpu_read32(gpu, NV_FSP_MSG_QUEUE_TAIL);

	msg_size_words = (tail - head + 4) / 4;
	if (msg_size_words > max_count)
		msg_size_words = max_count;

	fsp_emem_read(gpu, NV_FSP_RPC_EMEM_BASE, data, msg_size_words);

	/* Reset message queue tail to acknowledge receipt */
	gpu_write32(gpu, NV_FSP_MSG_QUEUE_TAIL, head);

	return msg_size_words;
}

static void fsp_reset_rpc_state(struct vfio_pci_device *device)
{
	struct gpu_device *gpu = to_nv_gpu(device);
	u32 head, tail;

	head = gpu_read32(gpu, NV_FSP_QUEUE_HEAD);
	tail = gpu_read32(gpu, NV_FSP_QUEUE_TAIL);

	if (head == tail) {
		head = gpu_read32(gpu, NV_FSP_MSG_QUEUE_HEAD);
		tail = gpu_read32(gpu, NV_FSP_MSG_QUEUE_TAIL);
		if (head == tail)
			return;
	}

	fsp_poll_queue(gpu, NV_FSP_MSG_QUEUE_HEAD, NV_FSP_MSG_QUEUE_TAIL, false, 5000);

	gpu_write32(gpu, NV_FSP_QUEUE_TAIL, NV_FSP_RPC_EMEM_BASE);
	gpu_write32(gpu, NV_FSP_QUEUE_HEAD, NV_FSP_RPC_EMEM_BASE);
	gpu_write32(gpu, NV_FSP_MSG_QUEUE_TAIL, NV_FSP_RPC_EMEM_BASE);
	gpu_write32(gpu, NV_FSP_MSG_QUEUE_HEAD, NV_FSP_RPC_EMEM_BASE);
}

static inline u32 mctp_header_build(u8 seid, u8 seq, bool som, bool eom)
{
	u32 hdr = 0;

	hdr |= (seid & NV_MCTP_HDR_SEID_MASK) << NV_MCTP_HDR_SEID_SHIFT;
	hdr |= (seq & NV_MCTP_HDR_SEQ_MASK) << NV_MCTP_HDR_SEQ_SHIFT;
	if (som)
		hdr |= NV_MCTP_HDR_SOM_BIT;
	if (eom)
		hdr |= NV_MCTP_HDR_EOM_BIT;

	return hdr;
}

static inline u32 mctp_msg_header_build(u8 nvdm_type)
{
	u32 hdr = 0;

	hdr |= (NV_MCTP_MSG_TYPE_VENDOR_DEFINED & NV_MCTP_MSG_TYPE_MASK)
		<< NV_MCTP_MSG_TYPE_SHIFT;
	hdr |= (NV_MCTP_MSG_VENDOR_ID_NVIDIA & NV_MCTP_MSG_VENDOR_ID_MASK)
		<< NV_MCTP_MSG_VENDOR_ID_SHIFT;
	hdr |= (nvdm_type & NV_MCTP_MSG_NVDM_TYPE_MASK)
		<< NV_MCTP_MSG_NVDM_TYPE_SHIFT;

	return hdr;
}

static inline u8 mctp_msg_header_get_nvdm_type(u32 hdr)
{
	return (hdr >> NV_MCTP_MSG_NVDM_TYPE_SHIFT) & NV_MCTP_MSG_NVDM_TYPE_MASK;
}

static int fsp_rpc_send_cmd(struct vfio_pci_device *device, u8 nvdm_type,
			    const u32 *data, u32 data_count, u32 timeout_ms)
{
	struct gpu_device *gpu = to_nv_gpu(device);
	u32 max_packet_words = NV_FSP_RPC_MAX_PACKET_SIZE / 4;
	u32 packet[256];
	u32 resp_buf[256];
	u32 total_words;
	int resp_words;
	u8 resp_nvdm_type;
	int ret;

	total_words = 2 + data_count;
	if (total_words > max_packet_words)
		return -EINVAL;

	packet[0] = mctp_header_build(0, 0, true, true);
	packet[1] = mctp_msg_header_build(nvdm_type);

	if (data_count > 0)
		memcpy(&packet[2], data, data_count * sizeof(u32));

	ret = fsp_rpc_send_data(gpu, packet, total_words);
	if (ret)
		return ret;

	resp_words = fsp_rpc_receive_data(gpu, resp_buf, 256, timeout_ms);
	if (resp_words < 0)
		return resp_words;

	if (resp_words < NV_FSP_RPC_MIN_RESPONSE_WORDS)
		return -EPROTO;

	resp_nvdm_type = mctp_msg_header_get_nvdm_type(resp_buf[1]);
	if (resp_nvdm_type != NV_NVDM_TYPE_RESPONSE)
		return -EPROTO;

	if (resp_buf[3] != nvdm_type)
		return -EPROTO;

	if (resp_buf[4] != 0)
		return -resp_buf[4];

	return 0;
}

static void fsp_init(struct vfio_pci_device *device)
{
	gpu_poll_register(device, "fsp_boot_complete", NV_FSP_BOOT_COMPLETE_OFFSET,
			  NV_FSP_BOOT_COMPLETE_MASK, 0xffffffff, 5000);
	fsp_reset_rpc_state(device);
}

static int fsp_fbdma_enable(struct vfio_pci_device *device)
{
	struct gpu_device *gpu = to_nv_gpu(device);
	u32 cmd_data = NV_FBDMA_SUBCMD_ENABLE;
	int ret;

	if (gpu->fsp_dma_enabled)
		return 0;

	ret = fsp_rpc_send_cmd(device, NV_NVDM_TYPE_FBDMA, &cmd_data, 1, 5000);
	if (ret < 0)
		return ret;

	gpu->fsp_dma_enabled = true;
	return 0;
}

static bool fsp_check_ofa_dma_support(struct vfio_pci_device *device)
{
	struct gpu_device *gpu = to_nv_gpu(device);
	u32 val = gpu_read32(gpu, NV_OFA_DMA_SUPPORT_CHECK_REG);

	return (val >> 16) != 0xbadf;
}

static int size_to_dma_encoding(u64 size)
{
	size = min_t(u64, size, NV_FALCON_DMA_MAX_TRANSFER_SIZE);

	if (!size || (size & 0x3))
		return -EINVAL;

	return ffs(size) - 3;
}

static void gpu_enable_bus_master(struct vfio_pci_device *device)
{
	u16 cmd;

	cmd = vfio_pci_config_readw(device, PCI_COMMAND);
	vfio_pci_config_writew(device, PCI_COMMAND, cmd | PCI_COMMAND_MASTER);
}

static void gpu_disable_bus_master(struct vfio_pci_device *device)
{
	u16 cmd;

	cmd = vfio_pci_config_readw(device, PCI_COMMAND);
	vfio_pci_config_writew(device, PCI_COMMAND, cmd & ~PCI_COMMAND_MASTER);
}

static void falcon_dmem_port_configure(struct vfio_pci_device *device,
				       u32 offset, bool auto_inc_read,
				       bool auto_inc_write)
{
	struct gpu_device *gpu = to_nv_gpu(device);
	const struct falcon *falcon = gpu->falcon;
	u32 memc_value = offset;

	/* Set auto-increment flags */
	if (auto_inc_read)
		memc_value |= NV_PPWR_FALCON_DMEMC_AINCR_TRUE;
	if (auto_inc_write)
		memc_value |= NV_PPWR_FALCON_DMEMC_AINCW_TRUE;

	gpu_write32(gpu, falcon->dmem_control_reg, memc_value);
}

static void falcon_select_core_falcon(struct vfio_pci_device *device)
{
	struct gpu_device *gpu = to_nv_gpu(device);
	const struct falcon *falcon = gpu->falcon;
	u32 core_select_reg = falcon->base_page + NV_FALCON_CORE_SELECT_OFFSET;
	u32 core_select;

	/* Read current value */
	core_select = gpu_read32(gpu, core_select_reg);

	/* Clear bits 4:5 to select falcon core (not RISCV) */
	core_select &= ~NV_FALCON_CORE_SELECT_MASK;

	gpu_write32(gpu, core_select_reg, core_select);
}

static void falcon_enable(struct vfio_pci_device *device)
{
	struct gpu_device *gpu = to_nv_gpu(device);
	const struct falcon *falcon = gpu->falcon;
	u32 mailbox_test_reg;
	u32 mailbox_val;

	/* Ada-specific: Check if falcon needs reset before enable */
	if (gpu->arch == GPU_ARCH_ADA) {
		mailbox_test_reg = falcon->base_page + NV_FALCON_MAILBOX_TEST_OFFSET;
		mailbox_val = gpu_read32(gpu, mailbox_test_reg);
		if (mailbox_val == NV_FALCON_MAILBOX_RESET_MAGIC)
			gpu_write32(gpu, falcon->engine_reset, 1);
	}

	/* Enable the falcon based on control method */
	if (!falcon->no_outside_reset) {
		if (gpu->pmc_enable_mask != 0) {
			u32 pmc_enable;

			/* Enable via PMC_ENABLE register */
			pmc_enable = gpu_read32(gpu, NV_PMC_ENABLE);
			gpu_write32(gpu, NV_PMC_ENABLE, pmc_enable | gpu->pmc_enable_mask);
		} else {
			/* Enable by deasserting engine reset */
			gpu_write32(gpu, falcon->engine_reset, 0);
		}
	}

	if (gpu->arch < GPU_ARCH_HOPPER) {
		falcon_select_core_falcon(device);

		/* Wait for DMACTL to be ready (bits 1:2 should be 0) */
		gpu_poll_register(device, "falcon_dmactl", falcon->dmactl,
				  0, NV_FALCON_DMACTL_READY_MASK, 1000);
	}
}

static void falcon_disable(struct vfio_pci_device *device)
{
	struct gpu_device *gpu = to_nv_gpu(device);
	const struct falcon *falcon = gpu->falcon;
	u32 pmc_enable;

	if (falcon->no_outside_reset)
		return;

	if (gpu->pmc_enable_mask != 0) {
		/* Disable via PMC_ENABLE */
		pmc_enable = gpu_read32(gpu, NV_PMC_ENABLE);
		gpu_write32(gpu, NV_PMC_ENABLE, pmc_enable & ~gpu->pmc_enable_mask);
	} else {
		/* Disable by asserting engine reset */
		gpu_write32(gpu, falcon->engine_reset, 1);
	}
}

static void falcon_reset(struct vfio_pci_device *device)
{
	falcon_disable(device);

	falcon_enable(device);
}

static int nv_gpu_falcon_dma_init(struct vfio_pci_device *device)
{
	struct gpu_device *gpu = to_nv_gpu(device);
	const struct falcon *falcon;
	u32 transcfg;
	u32 dmactl;
	u32 ctl;
	int ret;

	falcon = gpu->falcon;

	gpu_enable_bus_master(device);

	if (gpu->arch >= GPU_ARCH_HOPPER) {
		fsp_init(device);

		ret = fsp_fbdma_enable(device);
		if (ret) {
			dev_err(device, "Failed to enable FSP FBDMA: %d\n", ret);
			return ret;
		}

		if (!fsp_check_ofa_dma_support(device)) {
			dev_err(device, "OFA DMA not supported with current firmware\n");
			return -EOPNOTSUPP;
		}
	}

	if (gpu->is_memory_clear_supported) {
		/* For Turing+, wait for boot to complete first */
		if (gpu->arch >= GPU_ARCH_TURING) {
			/* Wait for boot complete - Hopper+ uses FSP register */
			if (gpu->arch >= GPU_ARCH_HOPPER) {
				gpu_poll_register(device, "fsp_boot_complete",
					NV_FSP_BOOT_COMPLETE_OFFSET,
					NV_FSP_BOOT_COMPLETE_MASK, 0xffffffff, 5000);
			} else {
				gpu_poll_register(device, "boot_complete",
					NV_BOOT_COMPLETE_OFFSET,
					NV_BOOT_COMPLETE_MASK, 0xffffffff, 5000);
			}
			gpu_poll_register(device, "memory_clear_finished",
				NV_MEM_CLEAR_OFFSET, 0x1, 0xffffffff, 5000);
		}
	}

	if (!falcon->no_outside_reset)
		falcon_reset(device);

	falcon_dmem_port_configure(device, 0, false, false);

	transcfg = gpu_read32(gpu, falcon->fbif_transcfg);
	transcfg &= ~NV_FBIF_TRANSCFG_TARGET_MASK;
	transcfg |= NV_FBIF_TRANSCFG_SYSMEM_DEFAULT;
	gpu_write32(gpu, falcon->fbif_transcfg, transcfg);

	gpu_write32(gpu, falcon->fbif_ctl2, 0x1);

	ctl = gpu_read32(gpu, falcon->fbif_ctl);
	ctl |= NV_FBIF_CTL_ALLOW_PHYS_MODE | NV_FBIF_CTL_ALLOW_FULL_PHYS_MODE;
	gpu_write32(gpu, falcon->fbif_ctl, ctl);

	dmactl = gpu_read32(gpu, falcon->dmactl);
	dmactl &= ~NV_FALCON_DMACTL_DMEM_SCRUBBING;
	gpu_write32(gpu, falcon->dmactl, dmactl);

	return 0;
}

static int nv_gpu_falcon_dma(struct vfio_pci_device *device,
			     u64 address,
			     u32 size_encoding,
			     bool write)
{
	struct gpu_device *gpu = to_nv_gpu(device);
	const struct falcon *falcon = gpu->falcon;
	u32 dma_cmd;
	int ret;

	gpu_write32(gpu, NV_GPU_DMA_ADDR_TOP_BITS_REG,
		    (address >> 47) & 0x1ffff);
	gpu_write32(gpu, falcon->base_page + NV_FALCON_DMA_ADDR_HIGH_OFFSET,
		    (address >> 40) & 0x7f);
	gpu_write32(gpu, falcon->base_page + NV_FALCON_DMA_ADDR_LOW_OFFSET,
		    (address >> 8) & 0xffffffff);
	gpu_write32(gpu, falcon->base_page + NV_FALCON_DMA_BLOCK_OFFSET,
		    address & 0xff);
	gpu_write32(gpu, falcon->base_page + NV_FALCON_DMA_MEM_OFFSET, 0);

	dma_cmd = (size_encoding << NV_FALCON_DMA_CMD_SIZE_SHIFT);

	/* Set direction: write (DMEM->mem) or read (mem->DMEM) */
	if (write)
		dma_cmd |= NV_FALCON_DMA_CMD_WRITE_BIT;

	gpu_write32(gpu, falcon->base_page + NV_FALCON_DMA_CMD_OFFSET, dma_cmd);

	ret = gpu_poll_register(device, "dma_done",
		falcon->base_page + NV_FALCON_DMA_CMD_OFFSET,
		NV_FALCON_DMA_CMD_DONE_BIT, NV_FALCON_DMA_CMD_DONE_BIT,
		1000);
	if (ret)
		return ret;

	return 0;
}

static int nv_gpu_memcpy_chunk(struct vfio_pci_device *device,
				iova_t src,
				iova_t dst,
				u32 size_encoding)
{
	int ret;

	ret = nv_gpu_falcon_dma(device, src, size_encoding, false);
	if (ret) {
		dev_err(device, "Failed to queue DMA read (src=0x%llx, size=%u)\n",
			(unsigned long long)src, size_encoding);
		return ret;
	}

	ret = nv_gpu_falcon_dma(device, dst, size_encoding, true);
	if (ret) {
		dev_err(device, "Failed to queue DMA write (dst=0x%llx, size=%u)\n",
			(unsigned long long)dst, size_encoding);
		return ret;
	}

	return 0;
}

static int nv_gpu_probe(struct vfio_pci_device *device)
{
	enum gpu_arch gpu_arch;
	u32 pmc_boot_0;
	void *bar0;
	int i;

	if (vfio_pci_config_readw(device, PCI_VENDOR_ID) != PCI_VENDOR_ID_NVIDIA)
		return -ENODEV;

	if (vfio_pci_config_readw(device, PCI_CLASS_DEVICE) >> 8 !=
	    PCI_BASE_CLASS_DISPLAY)
		return -ENODEV;

	/* Get BAR0 pointer for reading GPU registers */
	bar0 = device->bars[0].vaddr;
	if (!bar0)
		return -ENODEV;

	/* Read PMC_BOOT_0 register from BAR0 to identify GPU */
	pmc_boot_0 = readl(bar0 + NV_PMC_BOOT_0);

	/* Look up GPU architecture to verify this is a supported GPU */
	gpu_arch = nv_gpu_arch_lookup(pmc_boot_0);
	if (gpu_arch == GPU_ARCH_UNKNOWN) {
		dev_err(device, "Unsupported GPU architecture for PMC_BOOT_0: 0x%x\n",
			pmc_boot_0);
		return -ENODEV;
	}

	/* Check verified GPU map */
	for (i = 0; i < VERIFIED_GPU_MAP_SIZE; i++) {
		if (verified_gpu_map[i] == pmc_boot_0)
			return 0;
	}

	dev_info(device, "Unvalidated GPU: PMC_BOOT_0: 0x%x, possibly not supported\n",
		pmc_boot_0);

	return 0;
}

static void nv_gpu_init(struct vfio_pci_device *device)
{
	struct gpu_device *gpu = to_nv_gpu(device);
	const struct gpu_properties *props;
	enum gpu_arch gpu_arch;
	u32 pmc_boot_0;
	int ret;

	VFIO_ASSERT_GE(device->driver.region.size, sizeof(*gpu));

	/* Read PMC_BOOT_0 register from BAR0 to identify GPU */
	pmc_boot_0 = readl(device->bars[0].vaddr + NV_PMC_BOOT_0);

	/* Look up GPU architecture */
	gpu_arch = nv_gpu_arch_lookup(pmc_boot_0);
	if (gpu_arch == GPU_ARCH_UNKNOWN) {
		dev_err(device, "Unsupported GPU architecture\n");
		return;
	}

	props = &gpu_properties_map[gpu_arch];

	/* Populate GPU structure */
	gpu->arch = gpu_arch;
	gpu->bar0 = device->bars[0].vaddr;
	gpu->is_memory_clear_supported = props->memory_clear_supported;
	gpu->falcon = &falcon_map[props->falcon_type];
	gpu->pmc_enable_mask = props->pmc_enable_mask;

	falcon_enable(device);

	/* Initialize falcon for DMA */
	ret = nv_gpu_falcon_dma_init(device);
	VFIO_ASSERT_EQ(ret, 0, "Failed to initialize falcon DMA: %d\n", ret);

	device->driver.features |= VFIO_PCI_DRIVER_F_NO_SEND_MSI;
	device->driver.max_memcpy_size = NV_FALCON_DMA_MAX_TRANSFER_SIZE;
	device->driver.max_memcpy_count = NV_FALCON_DMA_MAX_TRANSFER_COUNT;
}

static void nv_gpu_remove(struct vfio_pci_device *device)
{
	falcon_disable(device);
	gpu_disable_bus_master(device);
}

/*
 * Falcon DMA can only process one transfer at a time,
 * so the actual work is deferred to memcpy_wait() to conform to the
 * memcpy_start()/memcpy_wait() contract.
 */
static void nv_gpu_memcpy_start(struct vfio_pci_device *device,
				iova_t src, iova_t dst, u64 size, u64 count)
{
	struct gpu_device *gpu = to_nv_gpu(device);

	VFIO_ASSERT_EQ(gpu->memcpy_count, 0);

	gpu->memcpy_src = src;
	gpu->memcpy_dst = dst;
	gpu->memcpy_size = size;
	gpu->memcpy_count = count;
}

static int nv_gpu_memcpy_wait(struct vfio_pci_device *device)
{
	struct gpu_device *gpu = to_nv_gpu(device);
	u64 iteration;
	u64 offset;
	int ret;

	VFIO_ASSERT_NE(gpu->memcpy_count, 0);

	for (iteration = 0; iteration < gpu->memcpy_count; iteration++) {
		offset = 0;

		while (offset < gpu->memcpy_size) {
			int chunk_encoding;

			chunk_encoding = size_to_dma_encoding(
						gpu->memcpy_size - offset);
			if (chunk_encoding < 0) {
				ret = -EINVAL;
				goto out;
			}

			ret = nv_gpu_memcpy_chunk(device,
						  gpu->memcpy_src + offset,
						  gpu->memcpy_dst + offset,
						  chunk_encoding);
			if (ret)
				goto out;

			offset += 0x4 << chunk_encoding;
		}
	}

	ret = 0;
out:
	gpu->memcpy_count = 0;

	return ret;
}

const struct vfio_pci_driver_ops nv_falcon_ops = {
	.name = "nv_falcon",
	.probe = nv_gpu_probe,
	.init = nv_gpu_init,
	.remove = nv_gpu_remove,
	.memcpy_start = nv_gpu_memcpy_start,
	.memcpy_wait = nv_gpu_memcpy_wait,
};
