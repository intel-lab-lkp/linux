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

static int faux_probe_fail(struct faux_device *faux_dev)
{
	dev_info(&faux_dev->dev, "%s\n", __func__);
	return -ENODEV;
}

static struct faux_device_ops faux_ops_fail = {
	.probe = faux_probe_fail,
	.remove = faux_remove,
};

static int __init faux_test_init(void)
{
	fd1 = faux_device_create("fd1", NULL, NULL);
	if (!fd1) {
		pr_err("%s: fd1 creation failed\n", __func__);
		goto error_fd1;
	}

	fd2 = faux_device_create("fd2", &fd1->dev, &faux_ops);
	if (!fd2) {
		pr_err("%s: fd2 creation failed\n", __func__);
		goto error_fd2;
	}

	fd3 = faux_device_create("fd3", &fd2->dev, &faux_ops);
	if (!fd3) {
		pr_err("%s: fd3 creation failed\n", __func__);
		goto error_fd3;
	}

	struct faux_device *fd_bad = faux_device_create("fd4", &fd2->dev, &faux_ops_fail);
	if (fd_bad) {
		pr_err("%s: fd_bad creation succeeded\n", __func__);
		goto error_fd_bad;
	}

	return 0;
error_fd_bad:
	faux_device_destroy(fd3);
error_fd3:
	faux_device_destroy(fd2);
error_fd2:
	faux_device_destroy(fd1);
error_fd1:
	return -ENODEV;
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
