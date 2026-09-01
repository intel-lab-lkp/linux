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
 * each is a multicolor LED class device (asus-arion:led0..led3). A colour
 * change writes that LED's slot only: EFFECT 0x8160 + 3*led, DIRECT
 * 0x8100 + 3*led (3 bytes, byte order R, B, G), then APPLY (0x01) and
 * SAVE (0xaa). MODE (0x8021 = Static) is written first in every sequence;
 * skipping it makes the device ignore the whole sequence.
 *
 * Uses brightness_set (non-blocking LED core fast path) + a work_struct
 * for the sleeping block-request vendor CDB send.
 *
 * CDB length: scsi_execute_cmd() sizes the CDB via scsi_command_size(opcode),
 * which maps vendor opcode 0xec to 10 bytes. The ENE protocol uses a 16-byte
 * CDB with the data length in cdb[13], so scsi_execute_cmd() drops cdb[13]
 * and the device silently ignores the write. We build the request by hand
 * and force cmd_len = 16 (what SG_IO does from userspace).
 *
 * Attach manually until a notifier lands:
 * echo asus_aura > /sys/block/sdX/device/dh_state
 */

#include <linux/module.h>
#include <linux/slab.h>
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
	struct work_struct	work;
};

struct asus_aura_zone {
	struct scsi_device	*sdev;
	struct asus_aura_led	leds[ARION_NUM_LEDS];
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

/* Raw block request so we can force cmd_len=16 (see file header). */
static int ene_write(struct scsi_device *sdev, u16 reg,
		     const void *data, u8 arg_count)
{
	struct request *rq;
	struct scsi_cmnd *scmd;
	u8 cdb[ENE_CDB_LEN];
	int ret;

	ene_build_cdb(cdb, reg, arg_count);

	rq = blk_mq_alloc_request(sdev->request_queue, REQ_OP_DRV_OUT, 0);
	if (IS_ERR(rq))
		return PTR_ERR(rq);

	scmd = blk_mq_rq_to_pdu(rq);
	scmd->cmd_len = ENE_CDB_LEN;
	memcpy(scmd->cmnd, cdb, ENE_CDB_LEN);

	if (arg_count) {
		ret = blk_rq_map_kern(rq,
				      (void *)data, arg_count, GFP_KERNEL);
		if (ret)
			goto out;
	}

	blk_execute_rq(rq, true);
	ret = scmd->result;
out:
	blk_mq_free_request(rq);
	return ret;
}

/* Sleepable: runs on the system workqueue. Writes one LED's slot. */
static void asus_aura_led_work(struct work_struct *work)
{
	struct asus_aura_led *led =
		container_of(work, struct asus_aura_led, work);
	struct scsi_device *sdev = led->zone->sdev;
	u8 apply = ENE_APPLY;
	u8 save = ENE_SAVE;
	u8 mode = ENE_MODE_STATIC;
	int ret;

	if (!scsi_device_online(sdev))
		return;

	/* Mode first: without it the device ignores the whole sequence. */
	ret = ene_write(sdev, ENE_REG_MODE, &mode, 1);
	if (ret)
		goto err;

	ret = ene_write(sdev, ENE_REG_COLORS + led->index * ENE_RGB_LEN,
			led->rgb, ENE_RGB_LEN);
	if (ret)
		goto err;

	/*
	 * Cover the DIRECT colour set too; some firmware revisions pull
	 * from 0x8100 instead of 0x8160.
	 */
	ret = ene_write(sdev, ENE_REG_COLORS_DIRECT + led->index * ENE_RGB_LEN,
			led->rgb, ENE_RGB_LEN);
	if (ret)
		goto err;

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
		"asus_aura: led%d write failed: %d\n", led->index, ret);
}

/* Non-blocking LED callback (LED core fast path). Cache colour, defer SCSI. */
static void asus_aura_set(struct led_classdev *cdev,
			  enum led_brightness brightness)
{
	struct led_classdev_mc *mc = lcdev_to_mccdev(cdev);
	struct asus_aura_led *led =
		container_of(mc, struct asus_aura_led, mc_cdev);

	led_mc_calc_color_components(mc, brightness);
	/* ENE colour register byte order is R, B, G. */
	led->rgb[0] = led->subled[0].brightness;
	led->rgb[1] = led->subled[2].brightness;
	led->rgb[2] = led->subled[1].brightness;

	schedule_work(&led->work);
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

	cdev->name = kasprintf(GFP_KERNEL, "asus-arion:led%d", index);
	if (!cdev->name)
		return -ENOMEM;
	cdev->max_brightness = 255;
	cdev->brightness_set = asus_aura_set;

	INIT_WORK(&led->work, asus_aura_led_work);
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

	for (i = 0; i < ARION_NUM_LEDS; i++) {
		ret = asus_aura_register_led(zone, i);
		if (ret)
			goto err_free;
	}

	sdev->handler_data = zone;
	sdev_printk(KERN_INFO, sdev,
		    "asus_aura: %d per-LED multicolor LEDs registered\n",
		    ARION_NUM_LEDS);
	return SCSI_DH_OK;

err_free:
	while (i--) {
		cancel_work_sync(&zone->leds[i].work);
		led_classdev_multicolor_unregister(&zone->leds[i].mc_cdev);
		kfree(zone->leds[i].mc_cdev.led_cdev.name);
	}
	kfree(zone);
	return SCSI_DH_NOMEM;
}

static void asus_aura_detach(struct scsi_device *sdev)
{
	struct asus_aura_zone *zone = sdev->handler_data;
	int i;

	if (!zone)
		return;
	for (i = 0; i < ARION_NUM_LEDS; i++) {
		cancel_work_sync(&zone->leds[i].work);
		led_classdev_multicolor_unregister(&zone->leds[i].mc_cdev);
		kfree(zone->leds[i].mc_cdev.led_cdev.name);
	}
	kfree(zone);
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
