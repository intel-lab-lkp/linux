// SPDX-License-Identifier: GPL-2.0+
/*
 * ASUS Aura RGB over SCSI for ROG external NVMe enclosures
 * (e.g. ROG STRIX Arion, USB 0b05:1932).
 *
 * USB mass-storage device, no HID; the ENE LED controller is driven via
 * vendor SCSI commands. Matched by INQUIRY (vendor "ROG", model "ESD-S1C"),
 * does NOT claim the sdev (sd keeps owning the disk).
 *
 * The Arion exposes 4 independently addressable LEDs (verified on hardware):
 * each is a multicolor LED class device (asus-arion-<H:C:T:L>:led0..led3,
 * unique per enclosure). A colour change writes that LED's slot only:
 * EFFECT 0x8160 + 3*led, DIRECT 0x8100 + 3*led (3 bytes, byte order R, B, G),
 * then APPLY (0x01) and SAVE (0xaa). MODE (0x8021 = Static) is written first
 * in every sequence; skipping it makes the device ignore the whole sequence.
 *
 * Scheduling: brightness_set (LED core fast path) caches the colour and
 * marks the LED in a per-zone dirty mask under a spinlock; a single work
 * item per zone snapshots the mask and colours, then runs one ENE
 * sequence for all pending LEDs (MODE once, colour slots, APPLY, SAVE).
 * Funneling every update through that one work item also serializes the
 * sequences: the MODE/colour/APPLY/SAVE chain must never interleave
 * between concurrent LED updates. The snapshot makes re-queued runs with
 * nothing left to do return before touching the device, so a re-queue
 * cannot wear the flash with a pointless SAVE, and the lock keeps a
 * colour write from being reordered after its dirty bit on weakly
 * ordered architectures.
 *
 * CDB length: scsi_execute_cmd() sizes the CDB via COMMAND_SIZE(opcode),
 * which maps vendor opcode 0xec to 10 bytes. The ENE protocol uses a 16-byte
 * CDB with the data length in cdb[13], so scsi_execute_cmd() drops cdb[13]
 * and the device silently ignores the write. ene_write() therefore mirrors
 * scsi_execute_cmd() on top of scsi_alloc_request() and forces cmd_len = 16
 * (what SG_IO does from userspace).
 *
 * Attach manually until a notifier lands:
 * echo asus_aura > /sys/block/sdX/device/dh_state
 */

#include <linux/module.h>
#include <linux/bits.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/leds.h>
#include <linux/led-class-multicolor.h>
#include <linux/blk_types.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/workqueue.h>
#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_dh.h>

#define ARION_INQ_VENDOR	"ROG"
#define ARION_INQ_MODEL		"ESD-S1C"

#define ENE_OPCODE		0xec
#define ENE_REG_MODE		0x8021	/* AuraMode value: Static=1, Breathe=2, ... */
#define ENE_REG_APPLY		0x80a0
#define ENE_REG_COLORS		0x8160	/* + 3*led, 3 bytes per LED, order R,B,G */
#define ENE_REG_COLORS_DIRECT	0x8100	/* + 3*led, same layout */
#define ENE_APPLY		0x01
#define ENE_SAVE		0xaa
#define ENE_MODE_STATIC		1
#define ENE_CDB_LEN		16
#define ENE_RGB_LEN		3
#define ENE_TIMEOUT		(10 * HZ)

/*
 * Verified on hardware: the enclosure has 4 independently settable LEDs.
 * (The colour table reserves 16 slots; only the first 4 drive anything.)
 */
#define ARION_NUM_LEDS		4

struct asus_aura_led {
	struct asus_aura_zone	*zone;
	int			index;
	struct led_classdev_mc	mc_cdev;
	struct mc_subled	subled[3];
	u8			rgb[ENE_RGB_LEN];
};

struct asus_aura_zone {
	struct scsi_device	*sdev;
	struct asus_aura_led	leds[ARION_NUM_LEDS];
	spinlock_t		lock;	/* protects dirty and cached colours */
	u8			dirty;	/* bit i: led i needs a colour write */
	struct work_struct	work;
};

static void ene_build_cdb(u8 *cdb, u16 reg, u8 arg_count)
{
	memset(cdb, 0, ENE_CDB_LEN);
	cdb[0] = ENE_OPCODE;
	cdb[1] = 'A';
	cdb[2] = 'S';
	cdb[3] = (reg >> 8) & 0xff;
	cdb[4] = reg & 0xff;
	cdb[13] = arg_count;
}

/*
 * scsi_execute_cmd() with cmd_len forced to 16. scsi_alloc_request()
 * initializes the parts of the scsi_cmnd a passthrough needs (zeroed
 * cmnd, cmd_len = MAX_COMMAND_SIZE, sense_len, rcu head, retries);
 * a raw blk_mq_alloc_request() does none of that.
 */
static int ene_write(struct scsi_device *sdev, u16 reg,
		     const void *data, u8 arg_count)
{
	struct request *rq;
	struct scsi_cmnd *scmd;
	u8 cdb[ENE_CDB_LEN];
	int ret;

	ene_build_cdb(cdb, reg, arg_count);

	rq = scsi_alloc_request(sdev->request_queue, REQ_OP_DRV_OUT, 0);
	if (IS_ERR(rq))
		return PTR_ERR(rq);

	if (arg_count) {
		ret = blk_rq_map_kern(rq, (void *)data, arg_count, GFP_NOIO);
		if (ret)
			goto out;
	}

	scmd = blk_mq_rq_to_pdu(rq);
	scmd->cmd_len = ENE_CDB_LEN;
	memcpy(scmd->cmnd, cdb, ENE_CDB_LEN);
	scmd->allowed = 1;
	rq->timeout = ENE_TIMEOUT;
	rq->rq_flags |= RQF_QUIET;

	blk_execute_rq(rq, true);
	ret = scmd->result;
out:
	blk_mq_free_request(rq);
	return ret;
}

/*
 * Sleepable: runs on the system workqueue. One ENE sequence for every LED
 * marked in the dirty mask. The mask and colours are snapshotted under the
 * zone lock: asus_aura_set() may run concurrently on another CPU, and the
 * lock keeps a colour write from being reordered after its dirty bit on
 * weakly ordered architectures. A colour cached while this runs requeues
 * the work and is picked up by the next sequence.
 */
static void asus_aura_zone_work(struct work_struct *work)
{
	struct asus_aura_zone *zone =
		container_of(work, struct asus_aura_zone, work);
	struct scsi_device *sdev = zone->sdev;
	u8 rgb[ARION_NUM_LEDS][ENE_RGB_LEN];
	u8 apply = ENE_APPLY;
	u8 save = ENE_SAVE;
	u8 mode = ENE_MODE_STATIC;
	unsigned long flags;
	u8 pending;
	int i, ret;

	spin_lock_irqsave(&zone->lock, flags);
	pending = zone->dirty;
	zone->dirty = 0;
	for (i = 0; i < ARION_NUM_LEDS; i++)
		memcpy(rgb[i], zone->leds[i].rgb, ENE_RGB_LEN);
	spin_unlock_irqrestore(&zone->lock, flags);

	/*
	 * schedule_work() while this function runs requeues it, and the
	 * pending colour may already have been consumed above; the requeued
	 * run then has nothing to do. Return before touching the device:
	 * SAVE writes its flash.
	 */
	if (!pending)
		return;

	if (!scsi_device_online(sdev))
		return;

	/* Mode first: without it the device ignores the whole sequence. */
	ret = ene_write(sdev, ENE_REG_MODE, &mode, 1);
	if (ret)
		goto err;

	for (i = 0; i < ARION_NUM_LEDS; i++) {
		if (!(pending & BIT(i)))
			continue;

		ret = ene_write(sdev, ENE_REG_COLORS + i * ENE_RGB_LEN,
				rgb[i], ENE_RGB_LEN);
		if (ret)
			goto err;

		/*
		 * Cover the DIRECT colour set too; some firmware revisions
		 * pull from 0x8100 instead of 0x8160.
		 */
		ret = ene_write(sdev, ENE_REG_COLORS_DIRECT + i * ENE_RGB_LEN,
				rgb[i], ENE_RGB_LEN);
		if (ret)
			goto err;
	}

	ret = ene_write(sdev, ENE_REG_APPLY, &apply, 1);
	if (ret)
		goto err;

	/*
	 * The change only takes effect after SAVE (0xaa). NOTE: saving on
	 * every brightness change writes flash each time; revisit for wear
	 * once confirmed.
	 */
	ret = ene_write(sdev, ENE_REG_APPLY, &save, 1);
	if (ret)
		goto err;

	return;
err:
	dev_err(&sdev->sdev_gendev,
		"asus_aura: colour update failed: %d\n", ret);
}

/* Non-blocking LED callback (LED core fast path). Cache colour, defer SCSI. */
static void asus_aura_set(struct led_classdev *cdev,
			  enum led_brightness brightness)
{
	struct led_classdev_mc *mc = lcdev_to_mccdev(cdev);
	struct asus_aura_led *led =
		container_of(mc, struct asus_aura_led, mc_cdev);
	struct asus_aura_zone *zone = led->zone;
	unsigned long flags;

	led_mc_calc_color_components(mc, brightness);

	spin_lock_irqsave(&zone->lock, flags);
	/* ENE colour register byte order is R, B, G. */
	led->rgb[0] = led->subled[0].brightness;
	led->rgb[1] = led->subled[2].brightness;
	led->rgb[2] = led->subled[1].brightness;
	zone->dirty |= BIT(led->index);
	spin_unlock_irqrestore(&zone->lock, flags);

	schedule_work(&zone->work);
}

static int asus_aura_register_led(struct asus_aura_zone *zone, int index)
{
	struct asus_aura_led *led = &zone->leds[index];
	struct led_classdev *cdev = &led->mc_cdev.led_cdev;
	int ret;

	led->zone = zone;
	led->index = index;

	led->subled[0].color_index = LED_COLOR_ID_RED;
	led->subled[1].color_index = LED_COLOR_ID_GREEN;
	led->subled[2].color_index = LED_COLOR_ID_BLUE;
	led->mc_cdev.num_colors = 3;
	led->mc_cdev.subled_info = led->subled;

	/*
	 * Include the sdev's H:C:T:L: every enclosure gets its own SCSI
	 * host, so the names stay unique when more than one is connected.
	 * With a static name the LED core would register the second
	 * enclosure's LEDs under renamed nodes (asus-arion:led0_1), which
	 * is the wrong device identity. The names are per-attachment, like
	 * sd X letters, and userspace is expected to enumerate.
	 */
	cdev->name = kasprintf(GFP_KERNEL, "asus-arion-%s:led%d",
			       dev_name(&zone->sdev->sdev_gendev), index);
	if (!cdev->name)
		return -ENOMEM;
	cdev->max_brightness = 255;
	cdev->brightness_set = asus_aura_set;

	led_mc_calc_color_components(&led->mc_cdev, cdev->brightness);

	/*
	 * Register with NULL parent: parenting the LED to the sdev takes a
	 * device reference, which blocks the sdev's final release on unplug,
	 * which is what calls scsi_dh_release_device() -> our .detach() that
	 * unregisters the LEDs. That reference cycle leaked the LED nodes and
	 * the module refcount on every hot-unplug.
	 */
	ret = led_classdev_multicolor_register(NULL, &led->mc_cdev);
	if (ret)
		kfree(cdev->name);
	return ret;
}

/*
 * Unregister the LED devices before cancelling the work: unregistering
 * removes the sysfs attributes, so no new brightness_set can schedule the
 * zone work afterwards, and it waits for in-flight sysfs callbacks.
 * Cancelling first would leave a window where a brightness write requeues
 * the work after cancel_work_sync() returned, and the work would then run
 * on freed memory.
 */
static void asus_aura_release(struct asus_aura_zone *zone, int num_leds)
{
	int i;

	for (i = 0; i < num_leds; i++)
		led_classdev_multicolor_unregister(&zone->leds[i].mc_cdev);
	cancel_work_sync(&zone->work);
	for (i = 0; i < num_leds; i++)
		kfree(zone->leds[i].mc_cdev.led_cdev.name);
	kfree(zone);
}

static int asus_aura_attach(struct scsi_device *sdev)
{
	struct asus_aura_zone *zone;
	int i, ret;

	if (strncmp(sdev->vendor, ARION_INQ_VENDOR, strlen(ARION_INQ_VENDOR)) ||
	    strncmp(sdev->model, ARION_INQ_MODEL, strlen(ARION_INQ_MODEL)))
		return SCSI_DH_DEV_UNSUPP;

	zone = kzalloc_obj(*zone, GFP_KERNEL);
	if (!zone)
		return SCSI_DH_NOMEM;
	zone->sdev = sdev;
	spin_lock_init(&zone->lock);
	INIT_WORK(&zone->work, asus_aura_zone_work);

	for (i = 0; i < ARION_NUM_LEDS; i++) {
		ret = asus_aura_register_led(zone, i);
		if (ret) {
			asus_aura_release(zone, i);
			return SCSI_DH_NOMEM;
		}
	}

	sdev->handler_data = zone;
	return SCSI_DH_OK;
}

static void asus_aura_detach(struct scsi_device *sdev)
{
	struct asus_aura_zone *zone = sdev->handler_data;

	if (!zone)
		return;
	asus_aura_release(zone, ARION_NUM_LEDS);
	sdev->handler_data = NULL;
}

static struct scsi_device_handler asus_aura_dh = {
	.name	= "asus_aura",
	.module	= THIS_MODULE,
	.attach	= asus_aura_attach,
	.detach	= asus_aura_detach,
};

static int __init asus_aura_init(void)
{
	return scsi_register_device_handler(&asus_aura_dh);
}

static void __exit asus_aura_exit(void)
{
	scsi_unregister_device_handler(&asus_aura_dh);
}

module_init(asus_aura_init);
module_exit(asus_aura_exit);

MODULE_DESCRIPTION("ASUS Aura RGB over SCSI for ROG NVMe enclosures (per-LED)");
MODULE_AUTHOR("Liang Haowen");
MODULE_LICENSE("GPL");
