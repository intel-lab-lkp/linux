/* SPDX-License-Identifier: GPL-2.0+ */

/*
 * This is host-compiled generator for usb-ids.c (usb-storage)
 * and usb-ids-uas.c (uas).
 *
 * Generated files contain pre-computed 32-bit values, as USB
 * driver_info (where the value is stored) can be only 32-bit.
 * The most significant bit means that it is index to 64-bit
 * pre-computed table named usb_stor_drv_info_u64_table with size
 * stored in usb_stor_drv_info_u64_table_size (for sanity check).
 *
 * Note that usb-storage driver contains also UAS devices, while UAS
 * driver contains only UAS entries (so there can be duplicates).
 */

#include <stdio.h>
#include <string.h>

/*
 * Cannot use userspace <inttypes.h> as code below
 * processes internal kernel headers
 */
#include <linux/types.h>

/*
 * Silence warning for definitions in headers we do not use
 */
struct usb_device_id {};
struct usb_interface;

#include <linux/usb_usual.h>

typedef enum { TYPE_DEVICE_STORAGE, TYPE_DEVICE_UAS, TYPE_CLASS } dev_type;
typedef enum { FLAGS_NOT_SET, FLAGS_SET, FLAGS_DUPLICATE } dev_flags_set;
#define FLAGS_END (uint64_t)-1

struct unusual_dev_entry {
	dev_type type;

	/*interface */
	uint8_t bDeviceSubClass;
	uint8_t bDeviceProtocol;

	/* device */
	uint16_t idVendor;
	uint16_t idProduct;
	uint16_t bcdDevice_lo;
	uint16_t bcdDevice_hi;

	uint64_t flags;
	dev_flags_set set;
	unsigned int idx;
};

static struct unusual_dev_entry unusual_dev_entries[] = {
#define USUAL_DEV(useProto, useTrans) \
{ TYPE_CLASS, useProto, useTrans, 0, 0, 0, 0, 0, FLAGS_NOT_SET, 0 }

#define COMPLIANT_DEV  UNUSUAL_DEV
#define IS_ENABLED(x) 0

/* usb-storage */
#define UNUSUAL_DEV(id_vendor, id_product, bcdDeviceMin, bcdDeviceMax, \
		    vendorName, productName, useProtocol, useTransport, \
		    initFunction, flags) \
{ TYPE_DEVICE_STORAGE, 0, 0, id_vendor, id_product, bcdDeviceMin, bcdDeviceMax, flags, FLAGS_NOT_SET, 0 }
#include "unusual_devs.h"
#undef UNUSUAL_DEV

/* uas */
#define UNUSUAL_DEV(id_vendor, id_product, bcdDeviceMin, bcdDeviceMax, \
		    vendorName, productName, useProtocol, useTransport, \
		    initFunction, flags) \
{ TYPE_DEVICE_UAS, 0, 0, id_vendor, id_product, bcdDeviceMin, bcdDeviceMax, flags, FLAGS_NOT_SET, 0 }
#include "unusual_uas.h"
#undef UNUSUAL_DEV

/* Terminating entry */
{ .flags = FLAGS_END }
};
#undef USUAL_DEV
#undef COMPLIANT_DEV
#undef IS_ENABLED

/* Highest bit indicates it is index to usb_stor_drv_info_u64_table */
#define HI32 (uint32_t)(1UL << 31)

static uint64_t get_driver_info(uint64_t flags, unsigned int idx)
{
	if (CONFIG_64BIT)
		return flags;

	if (flags < HI32)
		return flags;

	/* Use index that will be processed in usb_stor_drv_info_to_flags */
	return HI32 + idx;
}

static void print_class(uint8_t bDeviceSubClass, uint8_t bDeviceProtocol)
{
	printf("\t{ .match_flags = USB_DEVICE_ID_MATCH_INT_INFO, ");
	printf(".bInterfaceClass = USB_CLASS_MASS_STORAGE, ");
	printf(".bInterfaceSubClass = 0x%x, .bInterfaceProtocol = 0x%x },\n",
		bDeviceSubClass, bDeviceProtocol);
}
static void print_type(dev_type type)
{
	for (int i = 0; unusual_dev_entries[i].flags != FLAGS_END; i++) {
		if (unusual_dev_entries[i].type != type)
			continue;

		if (type == TYPE_DEVICE_STORAGE || type == TYPE_DEVICE_UAS) {
			printf("\t{ .match_flags = USB_DEVICE_ID_MATCH_DEVICE_AND_VERSION, ");
			printf(".idVendor = 0x%04x, .idProduct =0x%04x, "
				".bcdDevice_lo = 0x%04x, .bcdDevice_hi = 0x%04x, .driver_info = 0x%llx },\n",
				unusual_dev_entries[i].idVendor, unusual_dev_entries[i].idProduct,
				unusual_dev_entries[i].bcdDevice_lo, unusual_dev_entries[i].bcdDevice_hi,
				get_driver_info(unusual_dev_entries[i].flags, unusual_dev_entries[i].idx));
		} else if (type == TYPE_CLASS)
			print_class(unusual_dev_entries[i].bDeviceSubClass, unusual_dev_entries[i].bDeviceProtocol);
	}
}

static void print_usb_flags(const char *type)
{
	int i, count;

	if (CONFIG_64BIT) {
		printf("const u64 usb_%s_drv_info_u64_table[] = {};\n", type);
		printf("const unsigned long usb_%s_drv_info_u64_table_size = 0;\n\n", type);
	} else {
		printf("const u64 usb_%s_drv_info_u64_table[] = {\n", type);
		for (i = 0, count = 0; unusual_dev_entries[i].flags != FLAGS_END; i++) {
			if (unusual_dev_entries[i].set == FLAGS_SET) {
				printf("\t/* 0x%02x */ 0x%llx,\n", unusual_dev_entries[i].idx, unusual_dev_entries[i].flags);
				count++;
			}
		}
		printf("};\n\n");
		printf("const unsigned long usb_%s_drv_info_u64_table_size = %i;\n\n", type, count);
	}
}

static void print_usb_storage(void)
{
	printf("#include <linux/usb.h>\n\n");

	/* conversion table from 32-bit device_flags to 64-bit flags */
	print_usb_flags("stor");

	/* usb_storage_usb_ids */
	printf("const struct usb_device_id usb_storage_usb_ids[] = {\n");

	/* usb-storage driver devices */
	print_type(TYPE_DEVICE_STORAGE);

	/* uas driver devices */
	printf("#if IS_ENABLED(CONFIG_USB_UAS)\n");
	print_type(TYPE_DEVICE_UAS);
	printf("#endif\n");

	/* transport subclasses */
	print_type(TYPE_CLASS);

	printf("\t{ } /* Terminating entry */\n};\n");
	printf("MODULE_DEVICE_TABLE(usb, usb_storage_usb_ids);\n");
}

static void print_usb_uas(void)
{
	printf("#include <linux/usb.h>\n\n");

	/* conversion table from 32-bit device_flags to 64-bit flags */
	print_usb_flags("uas");

	/* uas_usb_ids */
	printf("const struct usb_device_id uas_usb_ids[] = {\n");

	/* uas driver devices */
	print_type(TYPE_DEVICE_UAS);

	/* transport subclasses */
	print_class(USB_SC_SCSI, USB_PR_BULK);
	print_class(USB_SC_SCSI, USB_PR_UAS);

	printf("\t{ } /* Terminating entry */\n};\n");
	printf("MODULE_DEVICE_TABLE(usb, uas_usb_ids);\n");
}

int main(int argc, char *argv[])
{
	int i, j, idx = 0, idx_old, skip = 0;

	if (argc != 2 || (strcmp(argv[1], "storage") && strcmp(argv[1], "uas"))) {
		printf("Please specify output type: storage or uas.\n");
		return 1;
	}

	for (i = 0; unusual_dev_entries[i].flags != FLAGS_END; i++) {
		if (unusual_dev_entries[i].type == TYPE_CLASS)
			continue;
		skip = 0;
		if (unusual_dev_entries[i].flags >= HI32) {
			for (j = 0; j < i; j++) {
				if (unusual_dev_entries[j].flags == unusual_dev_entries[i].flags &&
				    unusual_dev_entries[j].set == FLAGS_SET) {
					skip = 1;
					idx_old = unusual_dev_entries[j].idx;
					break;
				}
			}
			if (skip) {
				unusual_dev_entries[i].idx = idx_old;
				unusual_dev_entries[i].set = FLAGS_DUPLICATE;
			} else {
				unusual_dev_entries[i].idx = idx;
				unusual_dev_entries[i].set = FLAGS_SET;
				idx++;
			}
		}
	}

	if (!strcmp(argv[1], "storage"))
		print_usb_storage();
	else if (!strcmp(argv[1], "uas"))
		print_usb_uas();
	else
		return 1;

	return 0;
}
