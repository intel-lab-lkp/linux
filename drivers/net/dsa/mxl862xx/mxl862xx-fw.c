// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Firmware flash and devlink support for MaxLinear MxL862xx
 *
 * Copyright (C) 2025 Daniel Golle <daniel@makrotopia.org>
 *
 * SB PDI - firmware download interface over clause-22 SMDIO
 * =========================================================
 *
 * The MxL862xx MCUboot loader accepts a firmware image through four "SB PDI"
 * registers in the switch SMDIO register space. It runs whenever no WSP
 * firmware is active: the normal firmware update enters it deliberately - the
 * SYS_MISC_FW_UPDATE API command sets a sticky rescue bit and reboots into
 * MCUboot - and the loader also stays here when the stored WSP firmware fails
 * its boot-time integrity check. This driver drives the loader's 0xc55c
 * "console" download path.
 *
 * SMDIO register access (mxl862xx_smdio_read/write):
 *   MII reg 0x1f := (<sb_pdi_reg> & 0xfff0)   ; page latch
 *   MII reg (<sb_pdi_reg> & 0x000f) := / => <u16 data>
 * so CTRL/ADDR/DATA/STAT (0xe100..0xe103) are MII regs 0/1/2/3 of page
 * 0xe100, not all reg 0x00.
 *
 * SB PDI registers (host name/addr  ->  MCU mailbox):
 *   CTRL 0xe100 -> 0xc0938400   mode: RST=0x00  RD=0x01  WR=0x02
 *   ADDR 0xe101 -> 0xc0938404   SB target word address (SB1 bank = 0x7800)
 *   DATA 0xe102 -> 0xc0938408   16-bit data / reply word
 *   STAT 0xe103 -> 0xc093840c   handshake: a magic (below) or a byte count
 *
 * STAT magics:
 *   READY  0xc55c   loader idle in the console loop        (this driver)
 *   DL_RDY 0xc33c   loader idle in the flashless loop
 *   START  0xf48f   host   -> begin download session
 *   ACK    0xf490   loader -> START acknowledged (START + 1)
 *   END    0x3cc3   host   -> end of transfer / finalise
 *   RDREG  0xe2c0   host   -> register-read command (| index), see below
 *
 * Console flash path (STAT=0xc55c) - mxl862xx_flash_firmware():
 *
 *   host                                   loader
 *   ----                                   ------
 *   reset (CTRL=ADDR=DATA=0)
 *   read STAT ............................ 0xc55c   (READY, idle)
 *   STAT := START(0xf48f)  -------------->
 *                          <-------------- STAT = 0xf490 (ACK)
 *   CTRL := WR
 *   DATA := hdr[0..9]  (20-byte header: type,size1,crc1,size2,crc2)
 *   reset; STAT := 20 (header len)  -----> parse hdr; r_remain=size1+size2;
 *                                          ERASE target region(s)
 *                          <-------------- STAT=21 (len+1), then STAT=0
 *                                          (erased)
 *   -- payload, streamed in slices: --
 *   CTRL := WR
 *   DATA := word x N ...
 *     at word 16384: reset; ADDR:=0x7800; CTRL:=WR   (half-bank -> SB1)
 *     at word 32760: flush slice:
 *        reset; STAT := <bytes_this_slice> ---> r_remain -= bytes; program
 *                          <------------------- STAT=0  (ready for next slice)
 *   ... repeat until the whole payload is sent ...
 *   STAT := END(0x3cc3)  ---------------------> finalise
 *
 * The r_remain == 0 rule (critical):
 *   Every host STAT write in the payload phase is a byte count; the loader
 *   does r_remain -= count and stays in the receive loop while r_remain != 0.
 *   It leaves the loop, validates, and - if it was in rescue - clears its
 *   rescue-enable bit so boot_go boots the new image, ONLY when r_remain hits
 *   EXACTLY 0. A count larger than r_remain underflows the 32-bit counter and
 *   wedges the loader until a power cycle. Hence:
 *     - never send a slice/chunk count larger than what is outstanding;
 *     - interrupted-download recovery feeds 1 byte at a time (see below).
 *
 * Interrupted-flash recovery (mxl862xx_rescue_drain):
 *   A host that dies mid-payload leaves the loader spinning in the slice loop
 *   holding STAT=0 (no magic). Feed single 1-byte chunks (one DATA word +
 *   STAT=1) until r_remain reaches 0, then STAT=END; the loader finalises the
 *   (now corrupt) image and re-arms READY for a clean reflash.
 *
 * Register-read challenge (non-destructive liveness proof):
 *   DATA := 0x7c23 (marker); STAT := 0xe2c0|idx
 *     -> loader returns a runtime word in DATA and re-arms STAT=0xc55c.
 *   The reply source is loader BSS, not a chip id; used only to prove a live
 *   mailbox in mxl862xx_rescue_mode_detect().
 *
 * The other STAT ready magic, 0xc33c, marks the loader's flashless
 * chip-to-chip download mode (MxL86281S 16-port tier); this driver does not
 * use it.
 *
 * Rescue lifecycle (devlink): probe runs mxl862xx_rescue_mode_detect(); a wedged
 * loader is drained back to READY by a background self-heal (rescue_heal_work) so
 * the multi-minute recovery never holds the devlink lock. devlink dev info
 * exposes the fw version (the "flashable" signal) only once at READY;
 * flash_update returns -EBUSY until then, and reprobes to WSP firmware on success.
 *
 * Notes:
 *   - Chip id/revision (0xc0d28884/88) are NOT reachable on this channel; they
 *     need the clause-45 MMD firmware mailbox, which is dead under MCUboot.
 *     Rescue identity is by SB PDI behaviour only (mxl862xx_rescue_mode_detect).
 *   - The SMDIO PHY address and the 0xe1xx offsets are OTP-configurable; derive
 *     them from the DT binding, do not assume fixed values.
 */

#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/overflow.h>
#include <linux/rtnetlink.h>
#include <linux/workqueue.h>
#include <net/dsa.h>
#include <net/switchdev.h>

#include "mxl862xx.h"
#include "mxl862xx-api.h"
#include "mxl862xx-cmd.h"
#include "mxl862xx-fw.h"
#include "mxl862xx-host.h"

/* SB PDI registers (clause-22 SMDIO address space) */
#define MXL862XX_SB_PDI_CTRL		0xe100
#define MXL862XX_SB_PDI_ADDR		0xe101
#define MXL862XX_SB_PDI_DATA		0xe102
#define MXL862XX_SB_PDI_STAT		0xe103

/* SB PDI CTRL modes */
#define MXL862XX_SB_PDI_CTRL_RST	0x00
#define MXL862XX_SB_PDI_CTRL_WR		0x02

/* SB PDI handshake magic (published/consumed via STAT) */
#define MXL862XX_SB_PDI_READY		0xc55c	/* loader idle, console loop */
#define MXL862XX_SB_PDI_DL_READY	0xc33c	/* loader idle, flashless loop */
#define MXL862XX_SB_PDI_START		0xf48f
#define MXL862XX_SB_PDI_END		0x3cc3
#define MXL862XX_SB_PDI_RDREG		0xe2c0	/* register-read cmd (| index) */
#define MXL862XX_SB_PDI_RDREG_MARK	0x7c23	/* marker placed in DATA for RDREG */

/* Behavioural presence probe: two distinct 16-bit latches on ADDR/DATA. */
#define MXL862XX_SB_PDI_PROBE_A		0x5a5a
#define MXL862XX_SB_PDI_PROBE_D		0xa5a5

/* Firmware transfer geometry */
#define MXL862XX_FW_HDR_SIZE		20
#define MXL862XX_FW_BANK_HALF		16384	/* words per half-bank */
#define MXL862XX_FW_BANK_SLICE		32760	/* words per full slice */
#define MXL862XX_FW_SB1_ADDR		0x7800	/* SB1 word address */

/* Timeouts (generous upper bounds) */
#define MXL862XX_FW_READY_TIMEOUT_MS	3000
#define MXL862XX_FW_ACK_TIMEOUT_MS	5000
#define MXL862XX_FW_ERASE_TIMEOUT_MS	300000
#define MXL862XX_FW_WRITE_TIMEOUT_MS	120000
#define MXL862XX_FW_REBOOT_DELAY_MS	5000
#define MXL862XX_FW_REPROBE_DELAY_MS	500
#define MXL862XX_RESCUE_READY_TIMEOUT_MS 1000

static int mxl862xx_sb_pdi_reset(struct mxl862xx_priv *priv)
{
	int ret;

	ret = mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_CTRL,
				   MXL862XX_SB_PDI_CTRL_RST);
	if (ret < 0)
		return ret;

	ret = mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_ADDR,
				   MXL862XX_SB_PDI_CTRL_RST);
	if (ret < 0)
		return ret;

	return mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_DATA,
				    MXL862XX_SB_PDI_CTRL_RST);
}

static int mxl862xx_sb_pdi_poll_stat(struct mxl862xx_priv *priv, u16 expected,
				     unsigned long timeout_ms)
{
	int ret, val;

	ret = read_poll_timeout(mxl862xx_smdio_read, val,
				val < 0 || (u16)val == expected,
				10000, timeout_ms * 1000, false,
				priv, MXL862XX_SB_PDI_STAT);
	if (val < 0)
		return val;
	return ret;
}

static int mxl862xx_sb_pdi_flush_slice(struct mxl862xx_priv *priv,
				       u32 data_written)
{
	int ret;

	ret = mxl862xx_sb_pdi_reset(priv);
	if (ret < 0)
		return ret;

	ret = mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_STAT, data_written);
	if (ret < 0)
		return ret;

	return mxl862xx_sb_pdi_poll_stat(priv, 0,
					 MXL862XX_FW_WRITE_TIMEOUT_MS);
}

static void mxl862xx_flash_notify(struct devlink *dl, const char *status,
				  u32 done, u32 total)
{
	devlink_flash_update_status_notify(dl, status, NULL, done, total);
}

/* Post-flash reprobe. device_reprobe() detaches the driver -- running
 * remove(), which frees priv -- then re-probes, so this work touches only its
 * own device and module references and frees itself. A re-probe failure leaves
 * the device unbound, exactly as a failed initial probe would, so it is only
 * logged. Running from a workqueue keeps device_reprobe() out of the devlink
 * caller's locking and signal context.
 */
struct mxl862xx_reprobe {
	struct delayed_work work;
	struct device *dev;
};

static void mxl862xx_reprobe_work_fn(struct work_struct *work)
{
	struct mxl862xx_reprobe *rp =
		container_of(work, struct mxl862xx_reprobe, work.work);
	struct device *dev = rp->dev;

	if (device_reprobe(dev))
		dev_err(dev, "reprobe failed; device left unbound\n");
	put_device(dev);
	kfree(rp);
	module_put(THIS_MODULE);
}

/* Allocate the reprobe up front, before the switch is disturbed, so an
 * allocation failure aborts cleanly. The caller holds a module and a device
 * reference; the work releases both once it runs. Returns NULL on -ENOMEM.
 */
static struct mxl862xx_reprobe *mxl862xx_reprobe_alloc(struct device *dev)
{
	struct mxl862xx_reprobe *rp;

	rp = kzalloc_obj(*rp);
	if (!rp)
		return NULL;
	rp->dev = dev;
	INIT_DELAYED_WORK(&rp->work, mxl862xx_reprobe_work_fn);
	return rp;
}

/* Byte-count of each chunk fed to the loader during drain. It MUST be 1: the
 * loader only lets us observe "counter == 0", never "counter < step", so any
 * step > 1 can subtract past zero, underflow the 32-bit counter and wedge the
 * loader for ~2^32 more bytes (a state only a power cycle clears). Stepping by
 * 1 walks the counter through every value and is guaranteed to land on zero
 * whatever its (possibly odd) start. A 1-byte chunk is a path the loader
 * already handles: the normal transfer ends with a single trailing byte for
 * odd-sized images (see Step 6).
 */
#define MXL862XX_DRAIN_CHUNK_BYTES	1

/* Poll STAT while draining a stuck download: 0 means "feed the next chunk",
 * READY means the loader left the receive loop and re-armed its command loop,
 * anything else is a transient (the count being consumed) - BUSY past the
 * window means the counter has hit zero and the loader is finalising.
 */
enum { MXL862XX_DRAIN_FEED, MXL862XX_DRAIN_READY, MXL862XX_DRAIN_BUSY };
static int mxl862xx_sb_pdi_poll_drain(struct mxl862xx_priv *priv,
				      unsigned long timeout_ms)
{
	int val;

	read_poll_timeout(mxl862xx_smdio_read, val,
			  val < 0 || (u16)val == MXL862XX_SB_PDI_READY ||
			  (u16)val == 0,
			  50, timeout_ms * 1000, false,
			  priv, MXL862XX_SB_PDI_STAT);
	if (val < 0)
		return val;
	if ((u16)val == MXL862XX_SB_PDI_READY)
		return MXL862XX_DRAIN_READY;
	if ((u16)val == 0)
		return MXL862XX_DRAIN_FEED;
	return MXL862XX_DRAIN_BUSY;
}

/* Recover a switch whose SB PDI download was interrupted mid-transfer - the
 * host died after MCUboot began erasing flash, whether it aborted mid erase or
 * mid image-write, both end up in the same place: the payload receive loop.
 * There the loader publishes STAT=0, waits for the host to write a byte-count
 * to STAT, DMAs that many bytes and subtracts the count from a remaining-bytes
 * counter, leaving the loop only when the counter reaches exactly zero. The
 * image size died with the host, so we feed single-byte chunks (see
 * MXL862XX_DRAIN_CHUNK_BYTES) to walk the counter to zero without underflow,
 * then send END. The loader validates the (now corrupt) image and re-arms
 * READY, or boots a valid image that happened to survive in flash. This only
 * finalises the transfer; the caller's reprobe classifies whichever state
 * results. Returns 0 once finalised, <0 on error. Does NOT recover a counter
 * already underflowed by an earlier oversized-chunk attempt - that needs a
 * power cycle.
 */
static int mxl862xx_rescue_drain(struct mxl862xx_priv *priv)
{
	struct device *dev = &priv->mdiodev->dev;
	/* Bound: twice the loader's 16 MiB image cap, one byte per chunk. */
	u32 max_chunks = 2u * (16u << 20) / MXL862XX_DRAIN_CHUNK_BYTES;
	u32 chunk = 0;
	int ret;

	while (chunk < max_chunks) {
		/* Teardown can interrupt this minutes-long drain. */
		if (test_bit(MXL862XX_FLAG_WORK_STOPPED, &priv->flags))
			return -ECANCELED;

		ret = mxl862xx_sb_pdi_poll_drain(priv, 2000);
		if (ret < 0)
			return ret;
		if (ret == MXL862XX_DRAIN_READY)
			return 0;
		if (ret == MXL862XX_DRAIN_BUSY)
			break;

		/* Feed one zero byte; reset cleared the write latch. */
		ret = mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_CTRL,
					   MXL862XX_SB_PDI_CTRL_WR);
		if (ret < 0)
			return ret;
		ret = mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_DATA, 0x0000);
		if (ret < 0)
			return ret;
		ret = mxl862xx_sb_pdi_reset(priv);
		if (ret < 0)
			return ret;
		ret = mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_STAT,
					   MXL862XX_DRAIN_CHUNK_BYTES);
		if (ret < 0)
			return ret;
		chunk++;
		cond_resched();
	}

	if (chunk >= max_chunks) {
		dev_err(dev,
			"flash: interrupted download did not drain after %u chunks\n",
			chunk);
		return -ETIMEDOUT;
	}

	ret = mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_STAT,
				   MXL862XX_SB_PDI_END);
	if (ret < 0)
		return ret;

	/* Let the loader validate and re-arm READY, or boot a surviving image;
	 * the caller's reprobe then classifies whichever state results.
	 */
	msleep(MXL862XX_FW_REBOOT_DELAY_MS);
	return 0;
}

/* Background self-heal: drain a wedged download off the devlink flash path, so
 * the minutes-long recovery never holds the devlink lock. Scheduled from probe;
 * reprobes on success so the probe-time detection re-classifies the switch.
 */
void mxl862xx_rescue_heal_work_fn(struct work_struct *work)
{
	struct mxl862xx_priv *priv =
		container_of(work, struct mxl862xx_priv, rescue_heal_work);
	struct device *dev = &priv->mdiodev->dev;
	struct mxl862xx_reprobe *ko;
	int ret;

	ret = mxl862xx_rescue_drain(priv);
	if (test_bit(MXL862XX_FLAG_WORK_STOPPED, &priv->flags))
		return;
	if (ret)
		return;

	/* The interrupted transfer is finalised; reprobe so the probe-time
	 * detection brings the driver up -- flashable in rescue mode if the
	 * loader is at READY, or normally if a valid image booted. The refs
	 * are released by the reprobe once it completes.
	 */
	if (!try_module_get(THIS_MODULE))
		return;
	get_device(dev);
	ko = mxl862xx_reprobe_alloc(dev);
	if (!ko) {
		put_device(dev);
		module_put(THIS_MODULE);
		return;
	}
	queue_delayed_work(system_long_wq, &ko->work,
			   msecs_to_jiffies(MXL862XX_FW_REPROBE_DELAY_MS));
}

/* Detect MCUboot rescue mode over clause-22 SMDIO alone, so the caller can rule
 * the loader out before any C45 API request (which spews CRC errors when no WSP
 * firmware answers). A scratch write to ADDR/DATA must latch or the chip is
 * absent (-ENODEV); STAT then classifies the state, poked destructively only
 * when 0, the one value a running firmware never holds:
 *
 *  - 0xc33c: flashless loop, ready.
 *  - 0xc55c: console loop, if the register-read challenge is serviced.
 *  - other non-zero: running firmware, left unpoked.
 *  - 0: wedged receive loop, if a 1-byte slice-advance drains back to 0.
 *
 * Return: MXL862XX_IN_RESCUE, MXL862XX_NOT_RESCUE, or negative (-ENODEV/SMDIO).
 */
int mxl862xx_rescue_mode_detect(struct mxl862xx_priv *priv)
{
	int stat, dat, ret, rb, a, d;

	/* rescue_ready gates flashing; a wedged loader needs the drain first. */
	priv->rescue_ready = false;

	/* Presence: a live chip latches the scratch write, an absent one floats. */
	a = mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_ADDR,
				 MXL862XX_SB_PDI_PROBE_A);
	if (a < 0)
		return a;
	d = mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_DATA,
				 MXL862XX_SB_PDI_PROBE_D);
	if (d < 0)
		return d;
	a = mxl862xx_smdio_read(priv, MXL862XX_SB_PDI_ADDR);
	if (a < 0)
		return a;
	d = mxl862xx_smdio_read(priv, MXL862XX_SB_PDI_DATA);
	if (d < 0)
		return d;
	if ((u16)a != MXL862XX_SB_PDI_PROBE_A ||
	    (u16)d != MXL862XX_SB_PDI_PROBE_D)
		return -ENODEV;

	ret = mxl862xx_sb_pdi_reset(priv);
	if (ret < 0)
		return ret;

	stat = mxl862xx_smdio_read(priv, MXL862XX_SB_PDI_STAT);
	if (stat < 0)
		return stat;

	/* Flashless-download loop (MxL86281S tier): this driver does not
	 * support it -- the console flash path expects READY. Treat it as an
	 * unusable configuration, like any other unsupported state.
	 */
	if ((u16)stat == MXL862XX_SB_PDI_DL_READY)
		return -EOPNOTSUPP;

	/* Console loop at READY: confirm the live mailbox with the register-read
	 * challenge (consumes the marker from DATA and re-arms READY).
	 */
	if ((u16)stat == MXL862XX_SB_PDI_READY) {
		ret = mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_DATA,
					   MXL862XX_SB_PDI_RDREG_MARK);
		if (ret < 0)
			return ret;
		ret = mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_STAT,
					   MXL862XX_SB_PDI_RDREG);
		if (ret < 0)
			return ret;
		rb = mxl862xx_sb_pdi_poll_stat(priv, MXL862XX_SB_PDI_READY,
					       MXL862XX_RESCUE_READY_TIMEOUT_MS);
		dat = mxl862xx_smdio_read(priv, MXL862XX_SB_PDI_DATA);
		if (dat < 0)
			return dat;
		mxl862xx_sb_pdi_reset(priv);
		if (!rb && (u16)dat != MXL862XX_SB_PDI_RDREG_MARK) {
			priv->rescue_ready = true;
			return MXL862XX_IN_RESCUE;
		}
		return -ENXIO;
	}

	/* Any other non-zero value is a running firmware, not a loader. */
	if (stat)
		return MXL862XX_NOT_RESCUE;

	/* STAT == 0: a wedged receive loop consumes a 1-byte slice-advance back
	 * to 0 (feed one DATA word first, like a drain chunk).
	 */
	ret = mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_CTRL,
				   MXL862XX_SB_PDI_CTRL_WR);
	if (ret < 0)
		return ret;
	ret = mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_DATA, 0x0000);
	if (ret < 0)
		return ret;
	ret = mxl862xx_sb_pdi_reset(priv);
	if (ret < 0)
		return ret;
	ret = mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_STAT, 1);
	if (ret < 0)
		return ret;
	rb = mxl862xx_sb_pdi_poll_stat(priv, 0, MXL862XX_RESCUE_READY_TIMEOUT_MS);
	if (!rb)
		return MXL862XX_IN_RESCUE;

	return -ENXIO;
}

/* MCUboot firmware image header */
struct mxl862xx_fw_hdr {
	__le32 image_type;
	__le32 image_size_1;
	__le32 image_checksum_1;
	__le32 image_size_2;
	__le32 image_checksum_2;
} __packed;

static int mxl862xx_flash_validate(struct mxl862xx_priv *priv,
				   const struct firmware *fw,
				   u32 *payload_size)
{
	const struct mxl862xx_fw_hdr *hdr;
	u32 size1, size2, total;
	const u8 *payload;
	u32 crc;

	if (fw->size < MXL862XX_FW_HDR_SIZE)
		return -EINVAL;

	hdr = (const struct mxl862xx_fw_hdr *)fw->data;
	payload = fw->data + MXL862XX_FW_HDR_SIZE;
	size1 = le32_to_cpu(hdr->image_size_1);
	size2 = le32_to_cpu(hdr->image_size_2);

	if (check_add_overflow(size1, size2, &total) ||
	    total > fw->size - MXL862XX_FW_HDR_SIZE) {
		dev_err(&priv->mdiodev->dev,
			"flash: firmware file too small for declared size\n");
		return -EINVAL;
	}

	if (!total) {
		dev_err(&priv->mdiodev->dev,
			"flash: firmware file with empty payload\n");
		return -EINVAL;
	}

	if (size1) {
		crc = ~crc32_le(~0U, payload, size1);
		if (crc != le32_to_cpu(hdr->image_checksum_1)) {
			dev_err(&priv->mdiodev->dev,
				"flash: image 1 CRC mismatch (got %08x, expected %08x)\n",
				crc, le32_to_cpu(hdr->image_checksum_1));
			return -EINVAL;
		}
	}

	if (size2) {
		crc = ~crc32_le(~0U, payload + size1, size2);
		if (crc != le32_to_cpu(hdr->image_checksum_2)) {
			dev_err(&priv->mdiodev->dev,
				"flash: image 2 CRC mismatch (got %08x, expected %08x)\n",
				crc, le32_to_cpu(hdr->image_checksum_2));
			return -EINVAL;
		}
	}

	*payload_size = total;

	return 0;
}

static int mxl862xx_flash_firmware(struct mxl862xx_priv *priv,
				   const struct firmware *fw,
				   u32 payload_size, struct devlink *dl)
{
	const u8 *payload = fw->data + MXL862XX_FW_HDR_SIZE;
	u32 word_idx = 0, data_written = 0, idx = 0;
	unsigned long next_notify = jiffies - 1;
	u16 word, fdata;
	int ret, i;

	/* Step 1: reboot the firmware into MCUboot rescue mode */
	if (!priv->rescue_mode) {
		ret = mxl862xx_api_wrap(priv, SYS_MISC_FW_UPDATE, NULL, 0,
					false, false);
		if (ret) {
			dev_err(&priv->mdiodev->dev,
				"flash: FW_UPDATE command failed: %pe\n",
				ERR_PTR(ret));
			return ret;
		}
	}

	/* Step 2: wait for bootloader ready */
	mxl862xx_flash_notify(dl, "Waiting for bootloader", 0, 0);
	ret = mxl862xx_sb_pdi_reset(priv);
	if (ret < 0)
		goto write_err;

	/* Failures from here on jump to end_magic, which just returns the
	 * error without signalling END -- see there.
	 */
	ret = mxl862xx_sb_pdi_poll_stat(priv, MXL862XX_SB_PDI_READY,
					MXL862XX_FW_READY_TIMEOUT_MS);
	if (ret) {
		dev_err(&priv->mdiodev->dev,
			"flash: bootloader not ready: %pe\n", ERR_PTR(ret));
		goto end_magic;
	}

	/* Step 3: start handshake */
	ret = mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_STAT,
				   MXL862XX_SB_PDI_START);
	if (ret < 0)
		goto write_err;

	ret = mxl862xx_sb_pdi_poll_stat(priv, MXL862XX_SB_PDI_START + 1,
					MXL862XX_FW_ACK_TIMEOUT_MS);
	if (ret) {
		dev_err(&priv->mdiodev->dev,
			"flash: start handshake failed: %pe\n", ERR_PTR(ret));
		goto end_magic;
	}

	/* Step 4: transfer image header */
	mxl862xx_flash_notify(dl, "Erasing flash", 0, 0);
	ret = mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_CTRL,
				   MXL862XX_SB_PDI_CTRL_WR);
	if (ret < 0)
		goto write_err;

	for (i = 0; i < MXL862XX_FW_HDR_SIZE / 2; i++) {
		word = fw->data[i * 2] |
		       ((u16)fw->data[i * 2 + 1] << 8);
		ret = mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_DATA, word);
		if (ret < 0)
			goto write_err;
	}

	ret = mxl862xx_sb_pdi_reset(priv);
	if (ret < 0)
		goto write_err;

	/* the byte count in STAT triggers the erase */
	ret = mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_STAT,
				   MXL862XX_FW_HDR_SIZE);
	if (ret < 0)
		goto write_err;

	/* ACK is byte count + 1 */
	ret = mxl862xx_sb_pdi_poll_stat(priv, MXL862XX_FW_HDR_SIZE + 1,
					MXL862XX_FW_ACK_TIMEOUT_MS);
	if (ret) {
		dev_err(&priv->mdiodev->dev,
			"flash: header ACK failed: %pe\n", ERR_PTR(ret));
		goto end_magic;
	}

	/* Step 5: wait for erase to complete */
	ret = mxl862xx_sb_pdi_poll_stat(priv, 0,
					MXL862XX_FW_ERASE_TIMEOUT_MS);
	if (ret) {
		dev_err(&priv->mdiodev->dev,
			"flash: erase timeout: %pe\n", ERR_PTR(ret));
		goto end_magic;
	}

	/* Step 6: transfer payload */
	ret = mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_CTRL,
				   MXL862XX_SB_PDI_CTRL_WR);
	if (ret < 0)
		goto write_err;

	while (idx < payload_size) {
		cond_resched();
		if (idx + 1 < payload_size) {
			fdata = payload[idx] |
				((u16)payload[idx + 1] << 8);
			idx += 2;
			data_written += 2;
		} else {
			fdata = payload[idx];
			idx++;
			data_written++;
		}

		ret = mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_DATA, fdata);
		if (ret < 0)
			goto write_err;
		word_idx++;

		if (idx >= payload_size) {
			ret = mxl862xx_sb_pdi_flush_slice(priv, data_written);
			break;
		}

		/* Half-bank boundary: switch to SB1 address */
		if (word_idx == MXL862XX_FW_BANK_HALF) {
			ret = mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_CTRL,
						   MXL862XX_SB_PDI_CTRL_RST);
			if (ret < 0)
				goto write_err;

			ret = mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_ADDR,
						   MXL862XX_FW_SB1_ADDR);
			if (ret < 0)
				goto write_err;

			ret = mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_CTRL,
						   MXL862XX_SB_PDI_CTRL_WR);
			if (ret < 0)
				goto write_err;
		} else if (word_idx >= MXL862XX_FW_BANK_SLICE) {
			ret = mxl862xx_sb_pdi_flush_slice(priv, data_written);
			if (ret) {
				dev_err(&priv->mdiodev->dev,
					"flash: write timeout at %u/%u: %pe\n",
					idx, payload_size, ERR_PTR(ret));
				goto end_magic;
			}
			word_idx = 0;
			data_written = 0;
			ret = mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_CTRL,
						   MXL862XX_SB_PDI_CTRL_WR);
			if (ret < 0)
				goto write_err;

			if (time_after(jiffies, next_notify)) {
				mxl862xx_flash_notify(dl, "Flashing", idx,
						      payload_size);
				next_notify = jiffies + msecs_to_jiffies(500);
			}
		}
	}

	if (ret) {
		dev_err(&priv->mdiodev->dev,
			"flash: final write timeout: %pe\n", ERR_PTR(ret));
		goto end_magic;
	}

	mxl862xx_flash_notify(dl, "Flashing", payload_size, payload_size);

	/* Success: the loader has left the receive loop at r_remain == 0 and
	 * is back in its command loop, where END(0x3cc3) is a finalise/boot
	 * request rather than a byte count. Signal it here -- and only here --
	 * to boot the freshly written image.
	 */
	ret = mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_STAT,
				   MXL862XX_SB_PDI_END);
	msleep(MXL862XX_FW_REBOOT_DELAY_MS);
	return ret;

write_err:
	dev_err(&priv->mdiodev->dev, "flash: SMDIO write failed: %pe\n",
		ERR_PTR(ret));
end_magic:
	/* A failure leaves the loader mid transfer; do not signal END (a STAT
	 * write is a byte count then, and END would be misread as one, risking
	 * a receive-counter underflow). Return the error; the caller reprobes.
	 */
	return ret;
}

int mxl862xx_devlink_info_get(struct dsa_switch *ds,
			      struct devlink_info_req *req,
			      struct netlink_ext_ack *extack)
{
	struct mxl862xx_priv *priv = ds->priv;
	char buf[16];
	int ret;

	/* No chip-id/revision in MCUboot (needs the firmware MMD mailbox). The
	 * fw version doubles as the "ready to flash" signal: report it only
	 * once the loader is at a clean READY, nothing while still draining.
	 */
	if (priv->rescue_mode) {
		if (!READ_ONCE(priv->rescue_ready))
			return 0;

		snprintf(buf, sizeof(buf), "%u.%u.%u",
			 priv->fw_version.major, priv->fw_version.minor,
			 priv->fw_version.revision);
		ret = devlink_info_version_running_put(req,
				DEVLINK_INFO_VERSION_GENERIC_FW, buf);
		if (ret)
			return ret;
		return devlink_info_version_stored_put(req,
				DEVLINK_INFO_VERSION_GENERIC_FW, buf);
	}

	/* A 0 part number means the CHIP ID read failed or the part is
	 * unfused; omit it rather than publish a bogus "0000" that fwupd
	 * would match firmware against -- it then falls back to the driver
	 * name.
	 */
	if (priv->asic_id) {
		snprintf(buf, sizeof(buf), "%04X", priv->asic_id);
		ret = devlink_info_version_fixed_put(req,
						     DEVLINK_INFO_VERSION_GENERIC_ASIC_ID,
						     buf);
		if (ret)
			return ret;

		snprintf(buf, sizeof(buf), "%u", priv->asic_rev);
		ret = devlink_info_version_fixed_put(req,
						     DEVLINK_INFO_VERSION_GENERIC_ASIC_REV,
						     buf);
		if (ret)
			return ret;
	}

	snprintf(buf, sizeof(buf), "%u.%u.%u",
		 priv->fw_version.major, priv->fw_version.minor,
		 priv->fw_version.revision);

	ret = devlink_info_version_running_put(req,
			DEVLINK_INFO_VERSION_GENERIC_FW, buf);
	if (ret)
		return ret;

	/* boots this image from its own flash: stored == running */
	return devlink_info_version_stored_put(req,
			DEVLINK_INFO_VERSION_GENERIC_FW, buf);
}

int mxl862xx_devlink_flash_update(struct dsa_switch *ds,
				  struct devlink_flash_update_params *params,
				  struct netlink_ext_ack *extack)
{
	struct mxl862xx_reprobe *ko;
	struct mxl862xx_priv *priv = ds->priv;
	struct dsa_port *dp;
	u32 payload_size;
	int ret, i;

	if (params->component) {
		NL_SET_ERR_MSG_MOD(extack, "component is not supported");
		return -EOPNOTSUPP;
	}

	ret = mxl862xx_flash_validate(priv, params->fw, &payload_size);
	if (ret) {
		NL_SET_ERR_MSG_MOD(extack, "firmware image validation failed");
		return ret;
	}

	/* Refuse to flash while the background self-heal is still draining. */
	if (priv->rescue_mode && !READ_ONCE(priv->rescue_ready)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "switch is recovering an interrupted download, retry shortly");
		return -EBUSY;
	}

	/* The references the reprobe work needs to restore normal operation
	 * must be held before the switch is disturbed; the work itself is
	 * scheduled only once the flash is done (see below).
	 */
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	get_device(ds->dev);

	/* Allocate the reprobe work before disturbing the switch, so an
	 * -ENOMEM here cannot strand it flashed but never reprobed.
	 */
	ko = mxl862xx_reprobe_alloc(ds->dev);
	if (!ko) {
		put_device(ds->dev);
		module_put(THIS_MODULE);
		return -ENOMEM;
	}

	if (priv->rescue_mode)
		dev_info(ds->dev,
			 "flash: flashing switch via MCUboot rescue mode\n");
	else
		dev_info(ds->dev, "flash: running firmware %u.%u.%u\n",
			 priv->fw_version.major, priv->fw_version.minor,
			 priv->fw_version.revision);

	/* Close ports while the firmware is still alive so the DSA core's
	 * MDB/FDB tracking is drained, and detach user ports so userspace
	 * cannot reopen them during the flash. The conduit is only closed,
	 * not detached: it belongs to the MAC driver. This driver binds a
	 * single switch with a direct host link and no cascade ports, so the
	 * conduit serves only this switch, and flashing it reboots the switch,
	 * which takes the tree down regardless.
	 */
	rtnl_lock();
	dsa_switch_for_each_user_port(dp, ds) {
		if (dp->user) {
			dev_close(dp->user);
			netif_device_detach(dp->user);
		}
	}
	dsa_switch_for_each_cpu_port(dp, ds)
		dev_close(dp->conduit);
	/* The bridge defers the STP state changes triggered by closing
	 * the ports; let them reach the firmware while it is still alive.
	 */
	switchdev_deferred_process();
	rtnl_unlock();

	mutex_lock_nested(&priv->mdiodev->bus->mdio_lock, MDIO_MUTEX_NESTED);
	priv->block_host = true;
	mutex_unlock(&priv->mdiodev->bus->mdio_lock);

	set_bit(MXL862XX_FLAG_WORK_STOPPED, &priv->flags);
	cancel_delayed_work_sync(&priv->stats_work);
	cancel_work_sync(&priv->crc_err_work);
	for (i = 0; i < ds->num_ports; i++)
		cancel_work_sync(&priv->ports[i].host_flood_work);

	ret = mxl862xx_flash_firmware(priv, params->fw, payload_size,
				      ds->devlink);
	if (ret)
		NL_SET_ERR_MSG_MOD(extack, "firmware transfer failed");

	if (!ret) {
		mutex_lock_nested(&priv->mdiodev->bus->mdio_lock,
				  MDIO_MUTEX_NESTED);
		priv->block_host = false;
		priv->rescue_mode = false;
		mutex_unlock(&priv->mdiodev->bus->mdio_lock);

		/* Refresh the cached versions so the flash update only
		 * completes once the new firmware is confirmed running and
		 * devlink dev info reports it. Must happen before setting
		 * skip_teardown, which discards all firmware API reads.
		 */
		ret = mxl862xx_wait_ready(ds);
		if (ret)
			NL_SET_ERR_MSG_MOD(extack,
					   "new firmware did not become ready");
	}

	if (ret) {
		/* The switch is in MCUboot with erased or partly written flash;
		 * drop the cached identity so devlink dev info stops reporting
		 * the pre-flash version until the reprobe re-reads the truth.
		 */
		memset(&priv->fw_version, 0, sizeof(priv->fw_version));
		priv->asic_id = 0;
	}

	mutex_lock_nested(&priv->mdiodev->bus->mdio_lock, MDIO_MUTEX_NESTED);
	priv->skip_teardown = true;
	mutex_unlock(&priv->mdiodev->bus->mdio_lock);

	/* Queue the reprobe last; the work was allocated up front and its
	 * module and device references are already held.
	 */
	queue_delayed_work(system_long_wq, &ko->work,
			   msecs_to_jiffies(MXL862XX_FW_REPROBE_DELAY_MS));

	return ret;
}
