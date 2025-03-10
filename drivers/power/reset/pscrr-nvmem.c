// SPDX-License-Identifier: GPL-2.0
/*
 * pscrr_nvmem.c - PSCRR backend for storing shutdown reasons in small NVMEM
 *		   cells
 *
 * This backend provides a way to persist power state change reasons in a
 * non-volatile memory (NVMEM) cell, ensuring that reboot causes can be
 * analyzed post-mortem. Unlike traditional logging to eMMC or NAND, which
 * may be unreliable during power failures, this approach allows storing
 * reboot reasons in small, fast-access storage like RTC scratchpads, EEPROM,
 * or FRAM.
 *
 * The module allows dynamic configuration of the NVMEM device and cell
 * via module parameters:
 *
 * Example usage:
 *   modprobe pscrr-nvmem nvmem_name=pcf85063_nvram0 cell_name=pscr@0,0
 */

#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/pscrr.h>
#include <linux/slab.h>

/*
 * Module parameters:
 *   nvmem_name: Name of the NVMEM device (e.g. "pcf85063_nvram0").
 *   cell_name : Sysfs name of the cell on that device (e.g. "pscr@0,0").
 */
static char *nvmem_name;
module_param(nvmem_name, charp, 0444);
MODULE_PARM_DESC(nvmem_name, "Name of the NVMEM device (e.g. pcf85063_nvram0)");

static char *cell_name;
module_param(cell_name, charp, 0444);
MODULE_PARM_DESC(cell_name, "Sysfs name of the NVMEM cell (e.g. pscr@0,0)");

struct pscrr_nvmem_priv {
	struct nvmem_device *nvmem;
	struct nvmem_cell *cell;

	unsigned int total_bits;
	size_t max_val;
};

static struct pscrr_nvmem_priv *priv;

static int pscrr_nvmem_write_reason(enum pscr_reason reason)
{
	size_t required_bytes;
	u32 val;
	int ret;

	if (!priv || !priv->cell)
		return -ENODEV;

	/* Ensure reason fits in the available storage */
	if (reason > priv->max_val) {
		pr_err("PSCRR-nvmem: Reason %d exceeds max storable value %zu for %u-bit cell\n",
		       reason, priv->max_val, priv->total_bits);
		return -ERANGE;
	}

	val = reason;

	/* Determine required bytes for storing total_bits */
	required_bytes = (priv->total_bits + 7) / 8;

	/* Write the reason to the NVMEM cell */
	ret = nvmem_cell_write(priv->cell, &val, required_bytes);
	if (ret < 0) {
		pr_err("PSCRR-nvmem: Failed to write reason %d, err=%d (%pe)\n",
		       reason, ret, ERR_PTR(ret));
		return ret;
	}

	pr_debug("PSCRR-nvmem: Successfully wrote reason %d\n", reason);

	return 0;
}

static int pscrr_nvmem_read_reason(enum pscr_reason *reason)
{
	unsigned int required_bytes, val;
	int ret = 0;
	size_t len;
	void *buf;

	if (!priv || !priv->cell)
		return -ENODEV;

	buf = nvmem_cell_read(priv->cell, &len);
	if (IS_ERR(buf)) {
		ret = PTR_ERR(buf);
		pr_err("PSCRR-nvmem: Failed to read cell, err=%d (%pe)\n", ret,
		       ERR_PTR(ret));
		return ret;
	}

	/* Calculate the required number of bytes */
	required_bytes = (priv->total_bits + 7) / 8;

	/* Validate that the returned length is large enough */
	if (len < required_bytes) {
		pr_err("PSCRR-nvmem: Read length %zu is too small (need at least %u bytes)\n",
		       len, required_bytes);
		kfree(buf);
		return -EIO;
	}

	/* Extract value safely with proper memory alignment handling */
	val = 0;
	memcpy(&val, buf, required_bytes);

	/* Mask only the necessary bits to avoid garbage data */
	val &= (1U << priv->total_bits) - 1;

	kfree(buf);

	*reason = (enum pscr_reason)val;

	pr_debug("PSCRR-nvmem: Read reason => %d (from %zu bytes, %u bits used)\n",
		 *reason, len, priv->total_bits);

	return 0;
}

static const struct pscrr_backend_ops pscrr_nvmem_ops = {
	.write_reason = pscrr_nvmem_write_reason,
	.read_reason  = pscrr_nvmem_read_reason,
};

static int __init pscrr_nvmem_init(void)
{
	size_t bytes, bits;
	int ret;

	if (!nvmem_name || !cell_name) {
		pr_err("PSCRR-nvmem: Must specify both nvmem_name and cell_name.\n");
		return -EINVAL;
	}

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->nvmem = nvmem_device_get_by_name(nvmem_name);
	if (IS_ERR(priv->nvmem)) {
		ret = PTR_ERR(priv->nvmem);
		pr_err("PSCRR-nvmem: nvmem_device_get_by_name(%s) failed: %d\n",
		       nvmem_name, ret);
		priv->nvmem = NULL;
		goto err_free;
	}

	priv->cell = nvmem_cell_get_by_sysfs_name(priv->nvmem, cell_name);
	if (IS_ERR(priv->cell)) {
		ret = PTR_ERR(priv->cell);
		pr_err("PSCRR-nvmem: nvmem_cell_get_by_sysfs_name(%s) failed, err=%pe\n",
		       cell_name, ERR_PTR(ret));
		priv->cell = NULL;
		goto err_dev_put;
	}

	ret = nvmem_cell_get_size(priv->cell, &bytes, &bits);
	if (ret < 0) {
		pr_err("PSCRR-nvmem: Failed to get cell size, err=%pe\n",
		       ERR_PTR(ret));
		goto err_cell_put;
	}

	if (bits)
		priv->total_bits = bits;
	else
		priv->total_bits = bytes * 8;

	if (priv->total_bits > 31) {
		pr_err("PSCRR-nvmem: total_bits=%u is too large (max 31 allowed)\n",
		       priv->total_bits);
		return -EOVERFLOW;
	}

	priv->max_val = (1 << priv->total_bits) - 1;
	pr_debug("PSCRR-nvmem: Cell size: %zu bytes + %zu bits => total_bits=%u\n",
		 bytes, bits, priv->total_bits);

	/*
	 * If we store reasons 0..PSCR_MAX_REASON, the largest needed is
	 * 'PSCR_MAX_REASON'. That must fit within total_bits.
	 * So the max storable integer is (1 << total_bits) - 1.
	 */
	if (priv->max_val < PSCR_MAX_REASON) {
		pr_err("PSCRR-nvmem: Not enough bits (%u) to store up to reason=%d\n",
		       priv->total_bits, PSCR_MAX_REASON);
		ret = -ENOSPC;
		goto err_cell_put;
	}

	/* 4. Register with pscrr_core. */
	ret = pscrr_core_init(&pscrr_nvmem_ops);
	if (ret) {
		pr_err("PSCRR-nvmem: pscrr_core_init() failed: %d\n", ret);
		goto err_cell_put;
	}

	pr_info("PSCRR-nvmem: Loaded (nvmem=%s, cell=%s), can store 0..%zu\n",
		nvmem_name, cell_name, priv->max_val);
	return 0;

err_cell_put:
	if (priv->cell) {
		nvmem_cell_put(priv->cell);
		priv->cell = NULL;
	}
err_dev_put:
	if (priv->nvmem) {
		nvmem_device_put(priv->nvmem);
		priv->nvmem = NULL;
	}
err_free:
	kfree(priv);
	priv = NULL;
	return ret;
}

static void __exit pscrr_nvmem_exit(void)
{
	pscrr_core_exit();

	if (priv) {
		if (priv->cell) {
			nvmem_cell_put(priv->cell);
			priv->cell = NULL;
		}
		if (priv->nvmem) {
			nvmem_device_put(priv->nvmem);
			priv->nvmem = NULL;
		}
		kfree(priv);
		priv = NULL;
	}

	pr_info("pscrr-nvmem: Unloaded\n");
}

module_init(pscrr_nvmem_init);
module_exit(pscrr_nvmem_exit);

MODULE_AUTHOR("Oleksij Rempel <o.rempel@pengutronix.de>");
MODULE_DESCRIPTION("PSCRR backend for storing reason code in NVMEM");
MODULE_LICENSE("GPL");
