// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/of_reserved_mem.h>
#include <linux/elf.h>
#include <linux/firmware.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/firmware/qcom/qcom_tzmem.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/regulator/consumer.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <uapi/linux/fpc1540-tee.h>

#define FPC_TEE_MSG_SIZE	0x40
#define FPC_TEE_MAX_BULK	SZ_256K

struct fpc_tee {
	struct device *dev;
	struct miscdevice misc;
	struct regulator *vdd;
	struct gpio_desc *reset;
	struct gpio_desc *irq_gpio;
	struct qcom_tzmem_pool *pool;
	const char *ta_name;
	const char *lib_name;
	struct mutex lock;
	wait_queue_head_t irq_wait;
	atomic_t irq_events;
	bool powered;
	bool loaded;
	bool lib_loaded;
	u64 bridge;
	u32 app_id;
};

static irqreturn_t fpc_tee_irq(int irq, void *data)
{
	struct fpc_tee *fpc = data;

	if (fpc->irq_gpio && !gpiod_get_value_cansleep(fpc->irq_gpio))
		return IRQ_HANDLED;

	atomic_inc(&fpc->irq_events);
	wake_up_interruptible(&fpc->irq_wait);
	pm_wakeup_event(fpc->dev, 1000);

	return IRQ_HANDLED;
}

static void *fpc_tee_read_image(struct fpc_tee *fpc, const char *name,
				size_t *mdt_len, size_t *img_len,
				size_t *map_len)
{
	const struct firmware *fw;
	const struct elf32_hdr *ehdr;
	char fw_name[128];
	unsigned int phnum, i;
	size_t total, off;
	void *buf;
	int ret;

	snprintf(fw_name, sizeof(fw_name), "%s.mdt", name);
	ret = request_firmware(&fw, fw_name, fpc->dev);
	if (ret)
		return ERR_PTR(ret);

	ehdr = (const struct elf32_hdr *)fw->data;
	if (fw->size < sizeof(struct elf64_hdr) ||
	    memcmp(ehdr->e_ident, ELFMAG, SELFMAG)) {
		release_firmware(fw);
		return ERR_PTR(-EINVAL);
	}

	*map_len = 0;
	if (ehdr->e_ident[EI_CLASS] == ELFCLASS64) {
		const struct elf64_hdr *eh = (const struct elf64_hdr *)fw->data;
		const struct elf64_phdr *ph = (const void *)fw->data + eh->e_phoff;

		phnum = eh->e_phnum;
		if (eh->e_phoff + phnum * sizeof(*ph) <= fw->size)
			for (i = 0; i < phnum; i++)
				*map_len = max_t(size_t, *map_len,
						 ph[i].p_offset + ph[i].p_filesz);
	} else {
		const struct elf32_phdr *ph = (const void *)fw->data + ehdr->e_phoff;

		phnum = ehdr->e_phnum;
		if (ehdr->e_phoff + phnum * sizeof(*ph) <= fw->size)
			for (i = 0; i < phnum; i++)
				*map_len = max_t(size_t, *map_len,
						 ph[i].p_offset + ph[i].p_filesz);
	}

	*mdt_len = fw->size;
	total = fw->size;
	release_firmware(fw);

	for (i = 0; i < phnum; i++) {
		snprintf(fw_name, sizeof(fw_name), "%s.b%02u", name, i);
		ret = request_firmware(&fw, fw_name, fpc->dev);
		if (ret)
			return ERR_PTR(ret);
		total += fw->size;
		release_firmware(fw);
	}

	buf = vmalloc(total);
	if (!buf)
		return ERR_PTR(-ENOMEM);

	off = 0;
	for (i = 0; i <= phnum; i++) {
		if (i)
			snprintf(fw_name, sizeof(fw_name), "%s.b%02u", name, i - 1);
		else
			snprintf(fw_name, sizeof(fw_name), "%s.mdt", name);
		ret = request_firmware(&fw, fw_name, fpc->dev);
		if (ret || off + fw->size > total) {
			if (!ret)
				release_firmware(fw);
			vfree(buf);
			return ERR_PTR(ret ?: -EINVAL);
		}
		memcpy(buf + off, fw->data, fw->size);
		off += fw->size;
		release_firmware(fw);
	}

	*img_len = total;
	*map_len = max(*map_len, total);
	return buf;
}

static int fpc_tee_start(struct fpc_tee *fpc, phys_addr_t pa, size_t mdt_len,
			 size_t img_len, bool app)
{
	if (app)
		return qcom_scm_qseecom_app_start(pa, mdt_len, img_len, &fpc->app_id);

	return qcom_scm_qseecom_services_image_load(pa, mdt_len, img_len);
}

static int fpc_tee_load(struct fpc_tee *fpc, const char *name, bool app)
{
	size_t mdt_len, img_len, map_len, size;
	dma_addr_t pa;
	void *image, *buf;
	int ret;

	image = fpc_tee_read_image(fpc, name, &mdt_len, &img_len, &map_len);
	if (IS_ERR(image))
		return PTR_ERR(image);

	size = PAGE_ALIGN(map_len);
	buf = dma_alloc_coherent(fpc->dev, size, &pa, GFP_KERNEL);
	if (!buf) {
		vfree(image);
		return -ENOMEM;
	}

	memcpy(buf, image, img_len);
	vfree(image);

	ret = fpc_tee_start(fpc, pa, mdt_len, img_len, app);
	dma_free_coherent(fpc->dev, size, buf, pa);

	if (ret)
		dev_err(fpc->dev, "failed to start %s: %d\n", name, ret);

	return ret;
}

static int fpc_tee_ensure_loaded(struct fpc_tee *fpc)
{
	int ret;

	if (fpc->loaded)
		return 0;

	if (fpc->lib_name && !fpc->lib_loaded)
		fpc->lib_loaded = !fpc_tee_load(fpc, fpc->lib_name, false);

	ret = fpc_tee_load(fpc, fpc->ta_name, true);
	if (ret)
		return ret;

	fpc->loaded = true;
	return 0;
}

static int fpc_tee_power(struct fpc_tee *fpc, bool on)
{
	int ret;

	if (on == fpc->powered)
		return 0;

	if (on) {
		gpiod_set_value_cansleep(fpc->reset, 1);
		ret = regulator_enable(fpc->vdd);
		if (ret)
			return ret;
		usleep_range(1000, 1100);
		gpiod_set_value_cansleep(fpc->reset, 0);
	} else {
		gpiod_set_value_cansleep(fpc->reset, 1);
		usleep_range(1000, 1100);
		regulator_disable(fpc->vdd);
	}

	fpc->powered = on;
	return 0;
}

static void fpc_tee_hw_reset(struct fpc_tee *fpc)
{
	gpiod_set_value_cansleep(fpc->reset, 0);
	usleep_range(100, 200);
	gpiod_set_value_cansleep(fpc->reset, 1);
	usleep_range(5000, 5100);
	gpiod_set_value_cansleep(fpc->reset, 0);
	usleep_range(5000, 5100);
}

static int fpc_tee_cmd(struct fpc_tee *fpc, struct fpc_tee_cmd __user *ucmd)
{
	struct fpc_tee_cmd cmd;
	void *bulk, *req, *rsp;
	phys_addr_t bulk_phys;
	int ret;

	if (copy_from_user(&cmd, ucmd, sizeof(cmd)))
		return -EFAULT;
	if (!cmd.len || cmd.len > FPC_TEE_MAX_BULK)
		return -EINVAL;

	bulk = qcom_tzmem_alloc(fpc->pool, PAGE_ALIGN(cmd.len), GFP_KERNEL);
	req = qcom_tzmem_alloc(fpc->pool, FPC_TEE_MSG_SIZE, GFP_KERNEL);
	rsp = qcom_tzmem_alloc(fpc->pool, FPC_TEE_MSG_SIZE, GFP_KERNEL);
	if (!bulk || !req || !rsp) {
		ret = -ENOMEM;
		goto out;
	}

	memset(bulk, 0, PAGE_ALIGN(cmd.len));
	if (copy_from_user(bulk, u64_to_user_ptr(cmd.buf), cmd.len)) {
		ret = -EFAULT;
		goto out;
	}

	bulk_phys = qcom_tzmem_to_phys(bulk);
	if (upper_32_bits(bulk_phys)) {
		ret = -ENOMEM;
		goto out;
	}

	memset(req, 0, FPC_TEE_MSG_SIZE);
	memset(rsp, 0, FPC_TEE_MSG_SIZE);
	((u32 *)req)[0] = cmd.len;
	((u32 *)req)[1] = lower_32_bits(bulk_phys);

	ret = qcom_scm_qseecom_app_send(fpc->app_id, req, FPC_TEE_MSG_SIZE,
					rsp, FPC_TEE_MSG_SIZE);
	if (ret)
		goto out;

	cmd.status = ((s32 *)rsp)[0];
	if (copy_to_user(u64_to_user_ptr(cmd.buf), bulk, cmd.len) ||
	    copy_to_user(ucmd, &cmd, sizeof(cmd)))
		ret = -EFAULT;
out:
	if (bulk)
		qcom_tzmem_free(bulk);
	if (req)
		qcom_tzmem_free(req);
	if (rsp)
		qcom_tzmem_free(rsp);
	return ret;
}

static struct fpc_tee *fpc_tee_from_file(struct file *file)
{
	return container_of(file->private_data, struct fpc_tee, misc);
}

static int fpc_tee_open(struct inode *inode, struct file *file)
{
	struct fpc_tee *fpc = fpc_tee_from_file(file);
	int ret;

	mutex_lock(&fpc->lock);
	ret = fpc_tee_ensure_loaded(fpc);
	mutex_unlock(&fpc->lock);

	return ret;
}

static long fpc_tee_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct fpc_tee *fpc = fpc_tee_from_file(file);
	u32 on;
	long ret;

	mutex_lock(&fpc->lock);
	switch (cmd) {
	case FPC_TEE_IOC_POWER:
		ret = get_user(on, (u32 __user *)arg);
		if (!ret)
			ret = fpc_tee_power(fpc, on);
		break;
	case FPC_TEE_IOC_RESET:
		fpc_tee_hw_reset(fpc);
		ret = 0;
		break;
	case FPC_TEE_IOC_CMD:
		ret = fpc_tee_cmd(fpc, (struct fpc_tee_cmd __user *)arg);
		break;
	case FPC_TEE_IOC_IRQ_CLEAR:
		atomic_set(&fpc->irq_events, fpc->irq_gpio &&
			   gpiod_get_value_cansleep(fpc->irq_gpio) ? 1 : 0);
		ret = 0;
		break;
	default:
		ret = -ENOTTY;
	}
	mutex_unlock(&fpc->lock);

	return ret;
}

static __poll_t fpc_tee_poll(struct file *file, poll_table *wait)
{
	struct fpc_tee *fpc = fpc_tee_from_file(file);

	poll_wait(file, &fpc->irq_wait, wait);

	return atomic_read(&fpc->irq_events) ? EPOLLIN | EPOLLRDNORM : 0;
}

static ssize_t fpc_tee_read(struct file *file, char __user *buf, size_t len,
			    loff_t *ppos)
{
	struct fpc_tee *fpc = fpc_tee_from_file(file);
	u32 events;
	int ret;

	if (len < sizeof(events))
		return -EINVAL;

	if (!(file->f_flags & O_NONBLOCK)) {
		ret = wait_event_interruptible(fpc->irq_wait,
					       atomic_read(&fpc->irq_events));
		if (ret)
			return ret;
	}

	events = atomic_xchg(&fpc->irq_events, 0);
	if (copy_to_user(buf, &events, sizeof(events)))
		return -EFAULT;

	return sizeof(events);
}

static const struct file_operations fpc_tee_fops = {
	.owner = THIS_MODULE,
	.open = fpc_tee_open,
	.unlocked_ioctl = fpc_tee_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.poll = fpc_tee_poll,
	.read = fpc_tee_read,
	.llseek = noop_llseek,
};

static void fpc_tee_release_mem(void *data)
{
	struct fpc_tee *fpc = data;

	qcom_tzmem_shm_bridge_delete(fpc->bridge);
	of_reserved_mem_device_release(fpc->dev);
}

static int fpc_tee_probe(struct platform_device *pdev)
{
	struct qcom_tzmem_pool_config pool_config = {
		.policy = QCOM_TZMEM_POLICY_ON_DEMAND,
		.max_size = SZ_32M,
	};
	struct device *dev = &pdev->dev;
	struct reserved_mem *rmem;
	struct device_node *np;
	struct fpc_tee *fpc;
	int irq, ret;

	fpc = devm_kzalloc(dev, sizeof(*fpc), GFP_KERNEL);
	if (!fpc)
		return -ENOMEM;

	fpc->dev = dev;
	mutex_init(&fpc->lock);
	init_waitqueue_head(&fpc->irq_wait);

	ret = of_property_read_string_index(dev->of_node, "firmware-name", 0,
					    &fpc->ta_name);
	if (ret)
		return dev_err_probe(dev, ret, "no firmware-name\n");
	of_property_read_string_index(dev->of_node, "firmware-name", 1,
				      &fpc->lib_name);

	fpc->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(fpc->vdd))
		return dev_err_probe(dev, PTR_ERR(fpc->vdd), "no vdd supply\n");

	fpc->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(fpc->reset))
		return dev_err_probe(dev, PTR_ERR(fpc->reset), "no reset gpio\n");

	fpc->irq_gpio = devm_gpiod_get_optional(dev, "irq", GPIOD_IN);
	if (IS_ERR(fpc->irq_gpio))
		return dev_err_probe(dev, PTR_ERR(fpc->irq_gpio), "bad irq gpio\n");

	np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!np)
		return dev_err_probe(dev, -EINVAL, "no memory region\n");
	rmem = of_reserved_mem_lookup(np);
	of_node_put(np);
	if (!rmem)
		return dev_err_probe(dev, -EINVAL, "bad memory region\n");

	ret = of_reserved_mem_device_init(dev);
	if (ret)
		return dev_err_probe(dev, ret, "no memory region\n");

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (!ret)
		ret = qcom_tzmem_shm_bridge_create(rmem->base, rmem->size,
						   &fpc->bridge);
	if (ret) {
		of_reserved_mem_device_release(dev);
		return dev_err_probe(dev, ret, "no shm bridge\n");
	}

	ret = devm_add_action_or_reset(dev, fpc_tee_release_mem, fpc);
	if (ret)
		return ret;

	fpc->pool = devm_qcom_tzmem_pool_new(dev, &pool_config);
	if (IS_ERR(fpc->pool))
		return dev_err_probe(dev, PTR_ERR(fpc->pool), "no tzmem pool\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_threaded_irq(dev, irq, NULL, fpc_tee_irq,
					IRQF_ONESHOT, dev_name(dev), fpc);
	if (ret)
		return ret;

	device_init_wakeup(dev, true);
	enable_irq_wake(irq);

	fpc->misc.minor = MISC_DYNAMIC_MINOR;
	fpc->misc.name = "fpc_tee";
	fpc->misc.fops = &fpc_tee_fops;
	fpc->misc.parent = dev;

	platform_set_drvdata(pdev, fpc);

	return misc_register(&fpc->misc);
}

static void fpc_tee_remove(struct platform_device *pdev)
{
	struct fpc_tee *fpc = platform_get_drvdata(pdev);

	misc_deregister(&fpc->misc);
	if (fpc->loaded)
		qcom_scm_qseecom_app_shutdown(fpc->app_id);
	fpc_tee_power(fpc, false);
}

static const struct of_device_id fpc_tee_of_match[] = {
	{ .compatible = "fpc,fpc1540-tee" },
	{}
};
MODULE_DEVICE_TABLE(of, fpc_tee_of_match);

static struct platform_driver fpc_tee_driver = {
	.probe = fpc_tee_probe,
	.remove = fpc_tee_remove,
	.driver = {
		.name = "fpc1540-tee",
		.of_match_table = fpc_tee_of_match,
	},
};
module_platform_driver(fpc_tee_driver);

MODULE_DESCRIPTION("FPC1540 fingerprint sensor driven through a QSEE trusted application");
MODULE_LICENSE("GPL");
