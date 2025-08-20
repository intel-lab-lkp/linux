
#include <linux/acpi.h>
#include <linux/cache_coherency.h>
#include <asm/cacheflush.h>

struct acpi_cache_control {
	struct cache_coherency_device ccd;
	struct acpi_device *acpi_dev;
};

static const guid_t testguid =
	GUID_INIT(0x61FDC7D5, 0x1468, 0x4807,
		0xB5, 0x65, 0x51, 0x5B, 0xF6, 0xB7, 0x53, 0x19);

static int acpi_cache_control_query(struct acpi_device *device)
{
	union acpi_object *out_obj;

	out_obj = acpi_evaluate_dsm(device->handle, &testguid, 1, 1, NULL);//&in_obj);
	if (out_obj->package.count < 4) {
		printk("Only partial capabilities received\n");
		return -EINVAL;
	}
	for (int i = 0; i < out_obj->package.count; i++)
		if (out_obj->package.elements[i].type != 1) {
			printk("Element %d not integer\n", i);
			return -EINVAL;
		}
	switch (out_obj->package.elements[0].integer.value) {
	case 0:
		printk("Supports range\n");
		break;
	case 1:
		printk("Full flush only\n");
		break;
	default:
		printk("unknown op type %llx\n",
			out_obj->package.elements[0].integer.value);
		break;
	}

	printk("Latency is %lld msecs\n",
		out_obj->package.elements[1].integer.value);
	printk("Min delay between calls is %lld msecs\n",
		out_obj->package.elements[2].integer.value);

	if (out_obj->package.elements[3].integer.value & BIT(0))
		printk("CLEAN_INVALIDATE\n");
	if (out_obj->package.elements[3].integer.value & BIT(1))
		printk("CLEAN\n");
	if (out_obj->package.elements[3].integer.value & BIT(2))
		printk("INVALIDATE\n");
	ACPI_FREE(out_obj);
	return 0;
}

static int acpi_cache_control_inval(struct acpi_device *device, u64 base, u64 size)
{
	union acpi_object *out_obj;
	union acpi_object in_array[] = {
		[0].integer = { ACPI_TYPE_INTEGER, base },
		[1].integer = { ACPI_TYPE_INTEGER, size },
		[2].integer = { ACPI_TYPE_INTEGER, 0 }, // Clean invalidate
	};
	union acpi_object in_obj = {
		.package = {
			.type = ACPI_TYPE_PACKAGE,
			.count = ARRAY_SIZE(in_array),
			.elements = in_array,
		},
	};

	out_obj = acpi_evaluate_dsm(device->handle, &testguid, 1, 2, &in_obj);
	ACPI_FREE(out_obj);
	return 0;
}

static int acpi_cc_wbinv(struct cache_coherency_device *ccd,
			 struct cc_inval_params *invp)
{
	struct acpi_cache_control *acpi_cc =
		container_of(ccd, struct acpi_cache_control, ccd);

	return acpi_cache_control_inval(acpi_cc->acpi_dev, invp->addr, invp->size);
}

static int acpi_cc_done(struct cache_coherency_device *ccd)
{
	/* Todo */
	return 0;
}

static const struct coherency_ops acpi_cc_ops = {
	.wbinv = acpi_cc_wbinv,
	.done = acpi_cc_done,
};

static int acpi_cache_control_add(struct acpi_device *device)
{
	struct acpi_cache_control *acpi_cc;
	int ret;

	ret = acpi_cache_control_query(device);
	if (ret)
		return ret;

	acpi_cc = cache_coherency_device_alloc(&acpi_cc_ops,
					       struct acpi_cache_control, ccd);
	if (!acpi_cc)
		return -ENOMEM;

	acpi_cc->acpi_dev = device;

	ret = cache_coherency_device_register(&acpi_cc->ccd);
	if (ret) {
		cache_coherency_device_free(&acpi_cc->ccd);
		return ret;
	}

	dev_set_drvdata(&device->dev, acpi_cc);
	return 0;
}

static void acpi_cache_control_del(struct acpi_device *device)
{
	struct acpi_cache_control *acpi_cc = dev_get_drvdata(&device->dev);

	cache_coherency_device_unregister(&acpi_cc->ccd);
	cache_coherency_device_free(&acpi_cc->ccd);
}

static const struct acpi_device_id acpi_cache_control_ids[] = {
	{ "ACPI0019" },
	{ }
};

MODULE_DEVICE_TABLE(acpi, acpi_cache_control_ids);

static struct acpi_driver acpi_cache_control_driver = {
	.name = "acpi_cache_control",
	.ids = acpi_cache_control_ids,
	.ops = {
		.add = acpi_cache_control_add,
		.remove = acpi_cache_control_del,
	},
};

module_acpi_driver(acpi_cache_control_driver);

MODULE_IMPORT_NS("CACHE_COHERENCY");
MODULE_AUTHOR("Jonathan Cameron <Jonathan.Cameron@huawei.com>");
MODULE_DESCRIPTION("HACKS HACKS HACKS");
MODULE_LICENSE("GPL");
