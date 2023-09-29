// SPDX-License-Identifier: GPL-2.0
/*
 * Amazon Nitro Secure Module HWRNG driver.
 *
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 */

#include <linux/nsm.h>
#include <linux/hw_random.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/virtio_ids.h>

struct nsm_rng_info {
	struct hwrng hwrng;
	struct virtio_device *vdev;
};

#define CBOR_TYPE_MASK  0xE0
#define CBOR_TYPE_MAP 0xA0
#define CBOR_TYPE_TEXT 0x60
#define CBOR_TYPE_ARRAY 0x40
#define CBOR_HEADER_SIZE_SHORT 1

#define CBOR_SHORT_SIZE_MAX_VALUE 23
#define CBOR_LONG_SIZE_U8  24
#define CBOR_LONG_SIZE_U16 25
#define CBOR_LONG_SIZE_U32 26
#define CBOR_LONG_SIZE_U64 27

#define CBOR_HEADER_SIZE_U8  (CBOR_HEADER_SIZE_SHORT + sizeof(u8))
#define CBOR_HEADER_SIZE_U16 (CBOR_HEADER_SIZE_SHORT + sizeof(u16))
#define CBOR_HEADER_SIZE_U32 (CBOR_HEADER_SIZE_SHORT + sizeof(u32))
#define CBOR_HEADER_SIZE_U64 (CBOR_HEADER_SIZE_SHORT + sizeof(u64))

static bool cbor_object_is_array(const u8 *cbor_object, size_t cbor_object_size)
{
	if (cbor_object_size == 0 || cbor_object == NULL)
		return false;

	return (cbor_object[0] & CBOR_TYPE_MASK) == CBOR_TYPE_ARRAY;
}

static int cbor_object_get_array(u8 *cbor_object, size_t cbor_object_size, u8 **cbor_array)
{
	u8 cbor_short_size;
	u64 array_len;
	u64 array_offset;

	if (!cbor_object_is_array(cbor_object, cbor_object_size))
		return -EFAULT;

	if (cbor_array == NULL)
		return -EFAULT;

	cbor_short_size = (cbor_object[0] & 0x1F);

	/* Decoding byte array length */
	/* In short field encoding, the object header is 1 byte long and
	 * contains the type on the 3 MSB and the length on the LSB.
	 * If the length in the LSB is larger than 23, then the object
	 * uses long field encoding, and will contain the length over the
	 * next bytes in the object, depending on the value:
	 * 24 is u8, 25 is u16, 26 is u32 and 27 is u64.
	 */
	if (cbor_short_size <= CBOR_SHORT_SIZE_MAX_VALUE) {
		/* short encoding */
		array_len = cbor_short_size;
		array_offset = CBOR_HEADER_SIZE_SHORT;
	} else if (cbor_short_size == CBOR_LONG_SIZE_U8) {
		if (cbor_object_size < CBOR_HEADER_SIZE_U8)
			return -EFAULT;
		/* 1 byte */
		array_len = cbor_object[1];
		array_offset = CBOR_HEADER_SIZE_U8;
	} else if (cbor_short_size == CBOR_LONG_SIZE_U16) {
		if (cbor_object_size < CBOR_HEADER_SIZE_U16)
			return -EFAULT;
		/* 2 bytes */
		array_len = cbor_object[1] << 8 | cbor_object[2];
		array_offset = CBOR_HEADER_SIZE_U16;
	} else if (cbor_short_size == CBOR_LONG_SIZE_U32) {
		if (cbor_object_size < CBOR_HEADER_SIZE_U32)
			return -EFAULT;
		/* 4 bytes */
		array_len = cbor_object[1] << 24 |
			cbor_object[2] << 16 |
			cbor_object[3] << 8  |
			cbor_object[4];
		array_offset = CBOR_HEADER_SIZE_U32;
	} else if (cbor_short_size == CBOR_LONG_SIZE_U64) {
		if (cbor_object_size < CBOR_HEADER_SIZE_U64)
			return -EFAULT;
		/* 8 bytes */
		array_len = (u64) cbor_object[1] << 56 |
			  (u64) cbor_object[2] << 48 |
			  (u64) cbor_object[3] << 40 |
			  (u64) cbor_object[4] << 32 |
			  (u64) cbor_object[5] << 24 |
			  (u64) cbor_object[6] << 16 |
			  (u64) cbor_object[7] << 8  |
			  (u64) cbor_object[8];
		array_offset = CBOR_HEADER_SIZE_U64;
	}

	if (cbor_object_size < array_offset)
		return -EFAULT;

	if (cbor_object_size - array_offset < array_len)
		return -EFAULT;

	if (array_len > INT_MAX)
		return -EFAULT;

	*cbor_array = cbor_object + array_offset;
	return array_len;
}

static int nsm_rng_read(struct hwrng *rng, void *data, size_t max, bool wait)
{
	struct nsm_rng_info *nsm_rng_info = (struct nsm_rng_info *)rng;
	struct nsm_kernel_message message = {};
	int rc = 0;
	u8 *resp_ptr = NULL;
	u64 resp_len = 0;
	u8 *rand_data = NULL;
	/*
	 * 69                          # text(9)
	 *     47657452616E646F6D      # "GetRandom"
	 */
	const u8 request[] = { CBOR_TYPE_TEXT + strlen("GetRandom"),
			       'G', 'e', 't', 'R', 'a', 'n', 'd', 'o', 'm' };
	/*
	 * A1                          # map(1)
	 *     69                      # text(9) - Name of field
	 *         47657452616E646F6D  # "GetRandom"
	 * A1                          # map(1) - The field itself
	 *     66                      # text(6)
	 *         72616E646F6D        # "random"
	 *	# The rest of the response should be a byte array
	 */
	const u8 response[] = { CBOR_TYPE_MAP + 1,
				CBOR_TYPE_TEXT + strlen("GetRandom"),
				'G', 'e', 't', 'R', 'a', 'n', 'd', 'o', 'm',
				CBOR_TYPE_MAP + 1,
				CBOR_TYPE_TEXT + strlen("random"),
				'r', 'a', 'n', 'd', 'o', 'm' };

	/* NSM always needs to wait for a response */
	if (!wait)
		return 0;

	/* Set request message */
	message.request.iov_len = sizeof(request);
	message.request.iov_base = kmalloc(message.request.iov_len, GFP_KERNEL);
	if (message.request.iov_base == NULL)
		goto out;
	memcpy(message.request.iov_base, request, sizeof(request));

	/* Allocate space for response */
	message.response.iov_len = NSM_RESPONSE_MAX_SIZE;
	message.response.iov_base = kmalloc(message.response.iov_len, GFP_KERNEL);
	if (message.response.iov_base == NULL)
		goto out;

	/* Send/receive message */
	rc = nsm_communicate_with_device(nsm_rng_info->vdev, &message);
	if (rc != 0)
		goto out;

	resp_ptr = (u8 *) message.response.iov_base;
	resp_len = message.response.iov_len;

	if (resp_len < sizeof(response) + 1) {
		pr_err("NSM RNG: Received short response from NSM: Possible error message or invalid response");
		rc = -EFAULT;
		goto out;
	}

	if (memcmp(resp_ptr, response, sizeof(response)) != 0) {
		pr_err("NSM RNG: Invalid response header: Possible error message or invalid response");
		rc = -EFAULT;
		goto out;
	}

	resp_ptr += sizeof(response);
	resp_len -= sizeof(response);

	if (!cbor_object_is_array(resp_ptr, resp_len)) {
		/* not a byte array */
		pr_err("NSM RNG: Invalid response type: Expecting a byte array response");
		rc = -EFAULT;
		goto out;
	}

	rc = cbor_object_get_array(resp_ptr, resp_len, &rand_data);
	if (rc < 0) {
		pr_err("NSM RNG: Invalid CBOR encoding\n");
		goto out;
	}

	max = max > INT_MAX ? INT_MAX : max;
	rc = rc > max ? max : rc;
	memcpy(data, rand_data, rc);

	pr_debug("NSM RNG: returning rand bytes = %d\n", rc);
out:
	kfree(message.request.iov_base);
	kfree(message.response.iov_base);
	return rc;
}

static struct nsm_rng_info nsm_rng_info = {
	.hwrng = {
		.read = nsm_rng_read,
		.name = "nsm-hwrng",
		.quality = 1000,
	},
};

static int nsm_rng_probe(struct virtio_device *vdev)
{
	int rc;

	if (nsm_rng_info.vdev)
		return -EEXIST;

	nsm_rng_info.vdev = vdev;
	rc = devm_hwrng_register(&vdev->dev, &nsm_rng_info.hwrng);

	if (rc) {
		pr_err("NSM RNG initialization error: %d.\n", rc);
		return rc;
	}

	return 0;
}

static void nsm_rng_remove(struct virtio_device *vdev)
{
	if (nsm_rng_info.vdev != vdev)
		return;

	hwrng_unregister(&nsm_rng_info.hwrng);
	nsm_rng_info.vdev = NULL;
}

struct nsm_hwrng nsm_hwrng = {
	.probe = nsm_rng_probe,
	.remove = nsm_rng_remove,
};

static int __init nsm_rng_init(void)
{
	return nsm_register_hwrng(&nsm_hwrng);
}

static void __exit nsm_rng_exit(void)
{
	nsm_unregister_hwrng(&nsm_hwrng);
}

module_init(nsm_rng_init);
module_exit(nsm_rng_exit);

#ifdef MODULE
static const struct virtio_device_id nsm_id_table[] = {
	{ VIRTIO_ID_NITRO_SEC_MOD, VIRTIO_DEV_ANY_ID },
	{ 0 },
};
#endif

MODULE_DEVICE_TABLE(virtio, nsm_id_table);
MODULE_DESCRIPTION("Virtio NSM RNG driver");
MODULE_LICENSE("GPL");
