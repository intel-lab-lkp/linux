#include <linux/module.h>
#include <linux/device/faux.h>

static struct faux_device *fd1;
static struct faux_device *fd2;
static struct faux_device *fd3;

static int faux_probe(struct faux_device *faux_dev)
{
	dev_info(&faux_dev->dev, "%s\n", __func__);
	return 0;
}

static void faux_remove(struct faux_device *faux_dev)
{
	dev_info(&faux_dev->dev, "%s\n", __func__);
}

static struct faux_device_ops faux_ops = {
	.probe = faux_probe,
	.remove = faux_remove,
};

static int __init faux_test_init(void)
{
	fd1 = faux_device_create("fd1", NULL, NULL);
	fd2 = faux_device_create("fd2", &fd1->dev, &faux_ops);
	fd3 = faux_device_create("fd3", &fd2->dev, &faux_ops);
	return 0;
}

static void faux_test_exit(void)
{
	faux_device_destroy(fd3);
	faux_device_destroy(fd2);
	faux_device_destroy(fd1);
}

module_init(faux_test_init);
module_exit(faux_test_exit);
MODULE_LICENSE("GPL");
