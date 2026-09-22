// SPDX-License-Identifier: GPL-2.0-only
/*
 * Virtio cpufreq frontend
 *
 * Query the host for the frequency of the pCPU that currently runs a
 * given vCPU. Used when the guest cannot read host cpufreq sysfs, for
 * example a Xen domain that needs the value for energy accounting.
 *
 * The on-wire protocol is a packed { cpu_id, freq_khz } request that
 * must match the QEMU virtio-cpufreq backend.
 */

#include <linux/completion.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ids.h>

#define DRV_NAME "virtio-cpufreq"

struct virtio_cpufreq_req {
	__le32 cpu_id;
	__le32 freq_khz;
} __packed;

struct virtio_cpufreq {
	struct virtio_device *vdev;
	struct virtqueue *vq;
	struct mutex lock; /* serializes virtqueue requests */
};

static struct virtio_cpufreq *virtio_cpufreq_dev;

/*
 * Placeholder OPPs so cpufreq has a table. Real platforms should
 * populate this from the host or from firmware. Values match the
 * existing QEMU/Android backend used for bring-up.
 */
static struct cpufreq_frequency_table virtio_freq_table[] = {
	{ .frequency = 1400000 },
	{ .frequency = 1700000 },
	{ .frequency = 3000000 },
	{ .frequency = CPUFREQ_TABLE_END },
};

static int virtio_cpufreq_init_policy(struct cpufreq_policy *policy)
{
	cpumask_clear(policy->cpus);
	cpumask_set_cpu(policy->cpu, policy->cpus);

	policy->freq_table = virtio_freq_table;
	policy->cpuinfo.min_freq = 1400000;
	policy->cpuinfo.max_freq = 3000000;
	policy->cpuinfo.transition_latency = 0;

	return 0;
}

static int virtio_cpufreq_target_index(struct cpufreq_policy *policy,
				       unsigned int index)
{
	struct cpufreq_freqs freqs;

	freqs.old = policy->cur;
	freqs.new = policy->freq_table[index].frequency;

	/*
	 * This frontend does not change host frequency. It only keeps
	 * the cpufreq core in sync so userspace can observe values.
	 */
	cpufreq_freq_transition_begin(policy, &freqs);
	cpufreq_freq_transition_end(policy, &freqs, 0);

	return 0;
}

static unsigned int virtio_cpufreq_fallback(unsigned int cpu)
{
	struct cpufreq_policy *policy;
	unsigned int freq = 0;

	policy = cpufreq_cpu_get(cpu);
	if (policy) {
		freq = policy->cur;
		cpufreq_cpu_put(policy);
	}

	return freq;
}

static void virtio_cpufreq_vq_cb(struct virtqueue *vq)
{
	struct completion *done;
	unsigned int len;

	while ((done = virtqueue_get_buf(vq, &len)) != NULL)
		complete(done);
}

static unsigned int virtio_cpufreq_get(unsigned int cpu)
{
	struct virtio_cpufreq *vc = virtio_cpufreq_dev;
	struct virtio_cpufreq_req *req;
	struct scatterlist out_sg, in_sg, *sgs[2];
	struct completion done;
	unsigned int freq_khz;
	int ret;

	if (!vc || !vc->vq)
		return virtio_cpufreq_fallback(cpu);

	req = kzalloc_obj(*req, GFP_KERNEL);
	if (!req)
		return virtio_cpufreq_fallback(cpu);

	req->cpu_id = cpu_to_le32(cpu);

	init_completion(&done);
	sg_init_one(&out_sg, req, sizeof(*req));
	sg_init_one(&in_sg, req, sizeof(*req));
	sgs[0] = &out_sg;
	sgs[1] = &in_sg;

	mutex_lock(&vc->lock);
	ret = virtqueue_add_sgs(vc->vq, sgs, 1, 1, &done, GFP_KERNEL);
	if (ret) {
		mutex_unlock(&vc->lock);
		kfree(req);
		return virtio_cpufreq_fallback(cpu);
	}

	virtqueue_kick(vc->vq);
	ret = wait_for_completion_timeout(&done, msecs_to_jiffies(1000));
	mutex_unlock(&vc->lock);

	if (!ret) {
		/*
		 * The buffer may still be on the virtqueue. Leak it
		 * rather than freeing while the host may still write.
		 */
		return virtio_cpufreq_fallback(cpu);
	}

	freq_khz = le32_to_cpu(req->freq_khz);
	kfree(req);

	if (!freq_khz)
		return virtio_cpufreq_fallback(cpu);

	return freq_khz;
}

static struct freq_attr *virtio_cpufreq_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	NULL,
};

static struct cpufreq_driver virtio_cpufreq_driver = {
	.name		= DRV_NAME,
	.flags		= CPUFREQ_CONST_LOOPS,
	.init		= virtio_cpufreq_init_policy,
	.verify		= cpufreq_generic_frequency_table_verify,
	.target_index	= virtio_cpufreq_target_index,
	.get		= virtio_cpufreq_get,
	.attr		= virtio_cpufreq_attr,
};

static int virtio_cpufreq_probe(struct virtio_device *vdev)
{
	struct virtio_cpufreq *vc;
	struct virtqueue *vq;
	int ret;

	vc = devm_kzalloc(&vdev->dev, sizeof(*vc), GFP_KERNEL);
	if (!vc)
		return -ENOMEM;

	mutex_init(&vc->lock);
	vc->vdev = vdev;

	vq = virtio_find_single_vq(vdev, virtio_cpufreq_vq_cb, "requests");
	if (IS_ERR(vq))
		return PTR_ERR(vq);

	vc->vq = vq;
	vdev->priv = vc;
	virtio_cpufreq_dev = vc;
	virtio_device_ready(vdev);

	ret = cpufreq_register_driver(&virtio_cpufreq_driver);
	if (ret) {
		vdev->config->del_vqs(vdev);
		virtio_cpufreq_dev = NULL;
		return ret;
	}

	return 0;
}

static void virtio_cpufreq_remove(struct virtio_device *vdev)
{
	cpufreq_unregister_driver(&virtio_cpufreq_driver);
	virtio_cpufreq_dev = NULL;
	vdev->config->del_vqs(vdev);
}

static const struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_CPUFREQ, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static struct virtio_driver virtio_cpufreq_virtio_driver = {
	.driver.name	= DRV_NAME,
	.driver.owner	= THIS_MODULE,
	.id_table	= id_table,
	.probe		= virtio_cpufreq_probe,
	.remove		= virtio_cpufreq_remove,
};

static int __init virtio_cpufreq_mod_init(void)
{
	return register_virtio_driver(&virtio_cpufreq_virtio_driver);
}

static void __exit virtio_cpufreq_mod_exit(void)
{
	unregister_virtio_driver(&virtio_cpufreq_virtio_driver);
}

module_init(virtio_cpufreq_mod_init);
module_exit(virtio_cpufreq_mod_exit);

MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio cpufreq frontend");
MODULE_LICENSE("GPL");
