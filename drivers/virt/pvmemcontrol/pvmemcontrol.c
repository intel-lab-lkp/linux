// SPDX-License-Identifier: GPL-2.0
/*
 * Control guest physical memory properties by sending
 * madvise-esque requests to the host VMM.
 *
 * Author: Yuanchu Xie <yuanchu@google.com>
 * Author: Pasha Tatashin <pasha.tatashin@soleen.com>
 */
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/idr.h>
#include <linux/spinlock.h>
#include <linux/cpumask.h>
#include <linux/percpu-defs.h>
#include <linux/percpu.h>
#include <linux/types.h>
#include <linux/gfp.h>
#include <linux/compiler.h>
#include <linux/fs.h>
#include <linux/sched/clock.h>
#include <linux/wait.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/resource_ext.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/percpu.h>
#include <linux/byteorder/generic.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <uapi/linux/pvmemcontrol.h>

#define PCI_VENDOR_ID_GOOGLE 0x1ae0
#define PCI_DEVICE_ID_GOOGLE_PVMEMCONTROL 0x0087

#define PVMEMCONTROL_COMMAND_OFFSET 0x08
#define PVMEMCONTROL_REQUEST_OFFSET 0x00
#define PVMEMCONTROL_RESPONSE_OFFSET 0x00

static DEFINE_IDA(pvmemcontrol_minors_ida);

static unsigned int major_num __read_mostly;

struct pvmemcontrol;

struct pvmemcontrol_file_private {
	struct pvmemcontrol *dev;
	struct pvmemcontrol_buf buf;
};

/*
 * Magic values that perform the action specified when written to
 * the command register.
 */
enum pvmemcontrol_transport_command {
	PVMEMCONTROL_TRANSPORT_RESET = 0x060FE6D2,
	PVMEMCONTROL_TRANSPORT_REGISTER = 0x0E359539,
	PVMEMCONTROL_TRANSPORT_READY = 0x0CA8D227,
	PVMEMCONTROL_TRANSPORT_DISCONNECT = 0x030F5DA0,
	PVMEMCONTROL_TRANSPORT_ACK = 0x03CF5196,
	PVMEMCONTROL_TRANSPORT_ERROR = 0x01FBA249,
};

/* Contains the function code and arguments for specific function */
struct pvmemcontrol_vmm_call_le {
	__le64 func_code; /* pvmemcontrol set function code */
	__le64 addr; /* hyper. page size aligned guest phys. addr */
	__le64 length; /* hyper. page size aligned length */
	__le64 arg; /* function code specific argument */
};

/* Is filled on return to guest from VMM from most function calls */
struct pvmemcontrol_vmm_ret_le {
	__le32 ret_errno; /* on error, value of errno */
	__le32 ret_code; /* pvmemcontrol internal error code, on success 0 */
	__le64 ret_value; /* return value from the function call */
	__le64 arg0; /* currently unused */
	__le64 arg1; /* currently unused */
};

struct pvmemcontrol_buf_le {
	union {
		struct pvmemcontrol_vmm_call_le call;
		struct pvmemcontrol_vmm_ret_le ret;
	};
};

struct pvmemcontrol_percpu_channel {
	struct pvmemcontrol_buf_le buf;
	u64 buf_phys_addr;
	u32 command;
};

struct pvmemcontrol {
	int minor_number;
	void __iomem *base_addr;
	struct pci_dev *pci_dev;
	struct cdev cdev;
	struct device *cdev_device;
	/* cache the info call */
	struct pvmemcontrol_vmm_ret pvmemcontrol_vmm_info;
	struct pvmemcontrol_percpu_channel __percpu *pcpu_channels;
};

static void pvmemcontrol_write_command(void __iomem *base_addr, u32 command)
{
	iowrite32(command, base_addr + PVMEMCONTROL_COMMAND_OFFSET);
}

static u32 pvmemcontrol_read_command(void __iomem *base_addr)
{
	return ioread32(base_addr + PVMEMCONTROL_COMMAND_OFFSET);
}

static void pvmemcontrol_write_reg(void __iomem *base_addr, u64 buf_phys_addr)
{
	iowrite64_lo_hi(buf_phys_addr, base_addr + PVMEMCONTROL_REQUEST_OFFSET);
}

static u32 pvmemcontrol_read_resp(void __iomem *base_addr)
{
	return ioread32(base_addr + PVMEMCONTROL_RESPONSE_OFFSET);
}

static void pvmemcontrol_buf_call_to_le(struct pvmemcontrol_buf_le *le,
					const struct pvmemcontrol_buf *buf)
{
	le->call.func_code = cpu_to_le64(buf->call.func_code);
	le->call.addr = cpu_to_le64(buf->call.addr);
	le->call.length = cpu_to_le64(buf->call.length);
	le->call.arg = cpu_to_le64(buf->call.arg);
}

static void pvmemcontrol_buf_ret_from_le(struct pvmemcontrol_buf *buf,
					 const struct pvmemcontrol_buf_le *le)
{
	buf->ret.ret_errno = le32_to_cpu(le->ret.ret_errno);
	buf->ret.ret_code = le32_to_cpu(le->ret.ret_code);
	buf->ret.ret_value = le64_to_cpu(le->ret.ret_value);
	buf->ret.arg0 = le64_to_cpu(le->ret.arg0);
	buf->ret.arg1 = le64_to_cpu(le->ret.arg1);
}

static void pvmemcontrol_send_request(struct pvmemcontrol *pvmemcontrol,
				      struct pvmemcontrol_buf *buf)
{
	struct pvmemcontrol_percpu_channel *channel;

	preempt_disable();
	channel = this_cpu_ptr(pvmemcontrol->pcpu_channels);

	pvmemcontrol_buf_call_to_le(&channel->buf, buf);
	pvmemcontrol_write_command(pvmemcontrol->base_addr, channel->command);
	pvmemcontrol_buf_ret_from_le(buf, &channel->buf);

	preempt_enable();
}

static void pvmemcontrol_vmm_call(struct pvmemcontrol *pvmemcontrol,
				  struct pvmemcontrol_buf *buf)
{
	if (buf->call.func_code == PVMEMCONTROL_INFO) {
		memcpy(&buf->ret, &pvmemcontrol->pvmemcontrol_vmm_info,
		       sizeof(buf->ret));
		return;
	}

	pvmemcontrol_send_request(pvmemcontrol, buf);
}

static int pvmemcontrol_init_info(struct pvmemcontrol *dev,
				  struct pvmemcontrol_buf *buf)
{
	buf->call.func_code = PVMEMCONTROL_INFO;

	pvmemcontrol_send_request(dev, buf);
	if (buf->ret.ret_code)
		return buf->ret.ret_code;

	/* Initialize global pvmemcontrol_vmm_info */
	memcpy(&dev->pvmemcontrol_vmm_info, &buf->ret,
	       sizeof(dev->pvmemcontrol_vmm_info));
	dev_dbg(&dev->pci_dev->dev,
		"pvmemcontrol_vmm_info.ret_errno = %u\n"
		"pvmemcontrol_vmm_info.ret_code = %u\n"
		"pvmemcontrol_vmm_info.major_version = %llu\n"
		"pvmemcontrol_vmm_info.minor_version = %llu\n"
		"pvmemcontrol_vmm_info.page_size = %llu\n",
		dev->pvmemcontrol_vmm_info.ret_errno,
		dev->pvmemcontrol_vmm_info.ret_code,
		dev->pvmemcontrol_vmm_info.arg0,
		dev->pvmemcontrol_vmm_info.arg1,
		dev->pvmemcontrol_vmm_info.ret_value);

	return 0;
}

static int pvmemcontrol_open(struct inode *inode, struct file *filp)
{
	struct pvmemcontrol_file_private *priv = NULL;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	/* Do not allow exclusive open */
	if (filp->f_flags & O_EXCL)
		return -EINVAL;

	priv = kzalloc(sizeof(struct pvmemcontrol_file_private), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = container_of(inode->i_cdev, struct pvmemcontrol, cdev);
	filp->private_data = priv;
	return 0;
}

static int pvmemcontrol_release(struct inode *inode, struct file *filp)
{
	kfree(filp->private_data);
	filp->private_data = NULL;
	return 0;
}

static long pvmemcontrol_ioctl(struct file *filp, unsigned int cmd,
			       unsigned long ioctl_param)
{
	struct pvmemcontrol_file_private *priv = filp->private_data;
	struct pvmemcontrol *p = priv->dev;
	struct pvmemcontrol_buf *buf = &priv->buf;

	if (cmd != PVMEMCONTROL_IOCTL_VMM)
		return -EINVAL;

	if (copy_from_user(&buf->call, (void __user *)ioctl_param,
			   sizeof(struct pvmemcontrol_buf)))
		return -EFAULT;

	pvmemcontrol_vmm_call(p, buf);

	if (copy_to_user((void __user *)ioctl_param, &buf->ret,
			 sizeof(struct pvmemcontrol_buf)))
		return -EFAULT;

	return 0;
}

static const struct file_operations pvmemcontrol_fops = {
	.owner = THIS_MODULE,
	.open = pvmemcontrol_open,
	.release = pvmemcontrol_release,
	.unlocked_ioctl = pvmemcontrol_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static int pvmemcontrol_connect(struct pvmemcontrol *pvmemcontrol)
{
	int cpu;
	u32 cmd;

	pvmemcontrol_write_command(pvmemcontrol->base_addr,
				   PVMEMCONTROL_TRANSPORT_RESET);
	cmd = pvmemcontrol_read_command(pvmemcontrol->base_addr);
	if (cmd != PVMEMCONTROL_TRANSPORT_ACK) {
		dev_err(&pvmemcontrol->pci_dev->dev,
			"failed to reset device, cmd 0x%x\n", cmd);
		return -EINVAL;
	}

	for_each_possible_cpu(cpu) {
		struct pvmemcontrol_percpu_channel *channel =
			per_cpu_ptr(pvmemcontrol->pcpu_channels, cpu);

		pvmemcontrol_write_reg(pvmemcontrol->base_addr,
				       channel->buf_phys_addr);
		pvmemcontrol_write_command(pvmemcontrol->base_addr,
					   PVMEMCONTROL_TRANSPORT_REGISTER);

		cmd = pvmemcontrol_read_command(pvmemcontrol->base_addr);
		if (cmd != PVMEMCONTROL_TRANSPORT_ACK) {
			dev_err(&pvmemcontrol->pci_dev->dev,
				"failed to register pcpu buf, cmd 0x%x\n", cmd);
			return -EINVAL;
		}
		channel->command =
			pvmemcontrol_read_resp(pvmemcontrol->base_addr);
	}

	pvmemcontrol_write_command(pvmemcontrol->base_addr,
				   PVMEMCONTROL_TRANSPORT_READY);
	cmd = pvmemcontrol_read_command(pvmemcontrol->base_addr);
	if (cmd != PVMEMCONTROL_TRANSPORT_ACK) {
		dev_err(&pvmemcontrol->pci_dev->dev,
			"failed to ready device, cmd 0x%x\n", cmd);
		return -EINVAL;
	}
	return 0;
}

static int pvmemcontrol_disconnect(struct pvmemcontrol *pvmemcontrol)
{
	u32 cmd;

	pvmemcontrol_write_command(pvmemcontrol->base_addr,
				   PVMEMCONTROL_TRANSPORT_DISCONNECT);

	cmd = pvmemcontrol_read_command(pvmemcontrol->base_addr);
	if (cmd != PVMEMCONTROL_TRANSPORT_ERROR) {
		dev_err(&pvmemcontrol->pci_dev->dev,
			"failed to disconnect device, cmd 0x%x\n", cmd);
		return -EINVAL;
	}
	return 0;
}

static int pvmemcontrol_alloc_percpu_channels(struct pvmemcontrol *pvmemcontrol)
{
	int cpu;

	pvmemcontrol->pcpu_channels = alloc_percpu_gfp(
		struct pvmemcontrol_percpu_channel, GFP_ATOMIC | __GFP_ZERO);
	if (!pvmemcontrol->pcpu_channels)
		return -ENOMEM;

	for_each_possible_cpu(cpu) {
		struct pvmemcontrol_percpu_channel *channel =
			per_cpu_ptr(pvmemcontrol->pcpu_channels, cpu);
		phys_addr_t buf_phys = per_cpu_ptr_to_phys(&channel->buf);

		channel->buf_phys_addr = buf_phys;
	}
	return 0;
}

static const struct class pvmemcontrol_class = {
	.name = "pvmemcontrol",
};

static int pvmemcontrol_init(struct pci_dev *pci_dev, void __iomem *base_addr)
{
	struct pvmemcontrol_buf *buf = NULL;
	struct pvmemcontrol *pvmemcontrol = NULL;
	int err = 0;
	dev_t dev;
	int minor;

	pvmemcontrol = kzalloc(sizeof(struct pvmemcontrol), GFP_ATOMIC);
	buf = kzalloc(sizeof(struct pvmemcontrol_buf), GFP_ATOMIC);
	if (!pvmemcontrol || !buf) {
		err = -ENOMEM;
		goto fail_free;
	}

	minor = ida_alloc_max(&pvmemcontrol_minors_ida, MINORMASK, GFP_KERNEL);
	if (minor < 0) {
		err = minor;
		goto fail_free;
	}
	pvmemcontrol->minor_number = minor;
	pvmemcontrol->base_addr = base_addr;
	pvmemcontrol->pci_dev = pci_dev;

	cdev_init(&pvmemcontrol->cdev, &pvmemcontrol_fops);
	dev = MKDEV(major_num, minor);
	err = cdev_add(&pvmemcontrol->cdev, dev, 1);
	if (err)
		goto fail_minor;

	pvmemcontrol->cdev_device = device_create(
		&pvmemcontrol_class, NULL, dev, NULL, "pvmemcontrol%u", minor);
	if (IS_ERR(pvmemcontrol->cdev_device))
		goto fail_cdev;

	err = pvmemcontrol_alloc_percpu_channels(pvmemcontrol);
	if (err)
		goto fail_dev_create;

	err = pvmemcontrol_connect(pvmemcontrol);
	if (err)
		goto fail_free_percpu;

	err = pvmemcontrol_init_info(pvmemcontrol, buf);
	if (err)
		goto fail_disconnect;

	pci_set_drvdata(pci_dev, pvmemcontrol);
	kfree(buf);
	return 0;

fail_disconnect:
	pvmemcontrol_disconnect(pvmemcontrol);
fail_free_percpu:
	free_percpu(pvmemcontrol->pcpu_channels);
fail_dev_create:
	device_destroy(&pvmemcontrol_class, dev);
fail_cdev:
	cdev_del(&pvmemcontrol->cdev);
fail_minor:
	ida_free(&pvmemcontrol_minors_ida, minor);
fail_free:
	kfree(pvmemcontrol);
	kfree(buf);
	return err;
}

static int pvmemcontrol_pci_probe(struct pci_dev *pci_dev,
				  const struct pci_device_id *id)
{
	void __iomem *base_addr;
	int err;

	err = pcim_enable_device(pci_dev);
	if (err < 0)
		return err;

	base_addr = pcim_iomap(pci_dev, 0, 0);
	if (!base_addr)
		return -ENOMEM;

	err = pvmemcontrol_init(pci_dev, base_addr);
	return err;
}

static void pvmemcontrol_pci_remove(struct pci_dev *pci_dev)
{
	int err;
	struct pvmemcontrol *dev;

	dev = (struct pvmemcontrol *)pci_get_drvdata(pci_dev);
	if (!dev) {
		pci_err(pci_dev, "cleanup with no drvdata");
		return;
	}

	err = pvmemcontrol_disconnect(dev);
	if (err)
		dev_err(&pci_dev->dev, "device did not ack disconnect\n");
	/* free percpu channels */
	free_percpu(dev->pcpu_channels);
	device_destroy(&pvmemcontrol_class, dev->cdev.dev);
	cdev_del(&dev->cdev);
	ida_free(&pvmemcontrol_minors_ida, dev->minor_number);
	kfree(dev);
}

static const struct pci_device_id pvmemcontrol_pci_id_tbl[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_GOOGLE, PCI_DEVICE_ID_GOOGLE_PVMEMCONTROL) },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, pvmemcontrol_pci_id_tbl);

static struct pci_driver pvmemcontrol_pci_driver = {
	.name = "pvmemcontrol",
	.id_table = pvmemcontrol_pci_id_tbl,
	.probe = pvmemcontrol_pci_probe,
	.remove = pvmemcontrol_pci_remove,
};

static int __init pvmemcontrol_driver_init(void)
{
	int err;
	dev_t dev;

	err = class_register(&pvmemcontrol_class);
	if (err)
		return err;

	err = alloc_chrdev_region(&dev, 0, MINORMASK, "pvmemcontrol");
	if (err)
		goto fail_class;
	WRITE_ONCE(major_num, MAJOR(dev));

	err = pci_register_driver(&pvmemcontrol_pci_driver);
	if (err)
		goto fail_chrdev;

	return 0;
fail_chrdev:
	unregister_chrdev_region(major_num, MINORMASK);
fail_class:
	class_unregister(&pvmemcontrol_class);
	return err;
}

static void __exit pvmemcontrol_driver_exit(void)
{
	pci_unregister_driver(&pvmemcontrol_pci_driver);
	unregister_chrdev_region(major_num, MINORMASK);
	class_unregister(&pvmemcontrol_class);
}

module_init(pvmemcontrol_driver_init);
module_exit(pvmemcontrol_driver_exit);

MODULE_AUTHOR("Yuanchu Xie <yuanchu@google.com>");
MODULE_DESCRIPTION("pvmemcontrol Guest Service Module");
MODULE_LICENSE("GPL");
