// SPDX-License-Identifier: GPL-2.0
/*
 * Control guest physical memory properties by sending
 * madvise-esque requests to the host VMM.
 *
 * Author: Yuanchu Xie <yuanchu@google.com>
 * Author: Pasha Tatashin <pasha.tatashin@soleen.com>
 */
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
	void __iomem *base_addr;
	struct device *device;
	/* cache the info call */
	struct pvmemcontrol_vmm_ret pvmemcontrol_vmm_info;
	struct pvmemcontrol_percpu_channel __percpu *pcpu_channels;
};

static DEFINE_RWLOCK(pvmemcontrol_lock);
static struct pvmemcontrol *pvmemcontrol __read_mostly;

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

static int __pvmemcontrol_vmm_call(struct pvmemcontrol_buf *buf)
{
	int err = 0;

	if (!pvmemcontrol)
		return -EINVAL;

	read_lock(&pvmemcontrol_lock);
	if (!pvmemcontrol) {
		err = -EINVAL;
		goto unlock;
	}
	if (buf->call.func_code == PVMEMCONTROL_INFO) {
		memcpy(&buf->ret, &pvmemcontrol->pvmemcontrol_vmm_info,
		       sizeof(buf->ret));
		goto unlock;
	}

	pvmemcontrol_send_request(pvmemcontrol, buf);

unlock:
	read_unlock(&pvmemcontrol_lock);
	return err;
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
	dev_info(dev->device,
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
	struct pvmemcontrol_buf *buf = NULL;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	/* Do not allow exclusive open */
	if (filp->f_flags & O_EXCL)
		return -EINVAL;

	buf = kzalloc(sizeof(struct pvmemcontrol_buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/* Overwrite the misc device set by misc_register */
	filp->private_data = buf;
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
	struct pvmemcontrol_buf *buf = filp->private_data;
	int err;

	if (cmd != PVMEMCONTROL_IOCTL_VMM)
		return -EINVAL;

	if (copy_from_user(&buf->call, (void __user *)ioctl_param,
			   sizeof(struct pvmemcontrol_buf)))
		return -EFAULT;

	err = __pvmemcontrol_vmm_call(buf);
	if (err)
		return err;

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

static struct miscdevice pvmemcontrol_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = KBUILD_MODNAME,
	.fops = &pvmemcontrol_fops,
};

static int pvmemcontrol_connect(struct pvmemcontrol *pvmemcontrol)
{
	int cpu;
	u32 cmd;

	pvmemcontrol_write_command(pvmemcontrol->base_addr,
				   PVMEMCONTROL_TRANSPORT_RESET);
	cmd = pvmemcontrol_read_command(pvmemcontrol->base_addr);
	if (cmd != PVMEMCONTROL_TRANSPORT_ACK) {
		dev_err(pvmemcontrol->device,
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
			dev_err(pvmemcontrol->device,
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
		dev_err(pvmemcontrol->device,
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
		dev_err(pvmemcontrol->device,
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

static int pvmemcontrol_init(struct device *device, void __iomem *base_addr)
{
	struct pvmemcontrol_buf *buf = NULL;
	struct pvmemcontrol *dev = NULL;
	int err = 0;

	err = misc_register(&pvmemcontrol_dev);
	if (err)
		return err;

	/* We take a spinlock for a long time, but this is only during init. */
	write_lock(&pvmemcontrol_lock);
	if (READ_ONCE(pvmemcontrol)) {
		dev_warn(device, "multiple pvmemcontrol devices present\n");
		err = -EEXIST;
		goto fail_free;
	}

	dev = kzalloc(sizeof(struct pvmemcontrol), GFP_ATOMIC);
	buf = kzalloc(sizeof(struct pvmemcontrol_buf), GFP_ATOMIC);
	if (!dev || !buf) {
		err = -ENOMEM;
		goto fail_free;
	}

	dev->base_addr = base_addr;
	dev->device = device;

	err = pvmemcontrol_alloc_percpu_channels(dev);
	if (err)
		goto fail_free;

	err = pvmemcontrol_connect(dev);
	if (err)
		goto fail_free;

	err = pvmemcontrol_init_info(dev, buf);
	if (err)
		goto fail_free;

	WRITE_ONCE(pvmemcontrol, dev);
	write_unlock(&pvmemcontrol_lock);
	return 0;

fail_free:
	write_unlock(&pvmemcontrol_lock);
	kfree(dev);
	kfree(buf);
	misc_deregister(&pvmemcontrol_dev);
	return err;
}

static int pvmemcontrol_pci_probe(struct pci_dev *dev,
				  const struct pci_device_id *id)
{
	void __iomem *base_addr;
	int err;

	err = pcim_enable_device(dev);
	if (err < 0)
		return err;

	base_addr = pcim_iomap(dev, 0, 0);
	if (!base_addr)
		return -ENOMEM;

	err = pvmemcontrol_init(&dev->dev, base_addr);
	if (err)
		pci_disable_device(dev);

	return err;
}

static void pvmemcontrol_pci_remove(struct pci_dev *pci_dev)
{
	int err;
	struct pvmemcontrol *dev;

	write_lock(&pvmemcontrol_lock);
	dev = READ_ONCE(pvmemcontrol);
	if (!dev) {
		err = -EINVAL;
		dev_err(&pci_dev->dev, "cleanup called when uninitialized\n");
		write_unlock(&pvmemcontrol_lock);
		return;
	}

	/* disconnect */
	err = pvmemcontrol_disconnect(dev);
	if (err)
		dev_err(&pci_dev->dev, "device did not ack disconnect\n");
	/* free percpu channels */
	free_percpu(dev->pcpu_channels);

	kfree(dev);
	WRITE_ONCE(pvmemcontrol, NULL);
	write_unlock(&pvmemcontrol_lock);
	misc_deregister(&pvmemcontrol_dev);
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
module_pci_driver(pvmemcontrol_pci_driver);

MODULE_AUTHOR("Yuanchu Xie <yuanchu@google.com>");
MODULE_DESCRIPTION("pvmemcontrol Guest Service Module");
MODULE_LICENSE("GPL");
