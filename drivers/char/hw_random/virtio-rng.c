// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Randomness driver for virtio
 *  Copyright (C) 2007, 2008 Rusty Russell IBM Corporation
 */

#include <asm/barrier.h>
#include <linux/err.h>
#include <linux/hw_random.h>
#include <linux/scatterlist.h>
#include <linux/spinlock.h>
#include <linux/virtio.h>
#include <linux/virtio_rng.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/random.h>

static DEFINE_IDA(rng_index_ida);

struct virtrng_leak_queue {
	/* The underlying virtqueue of this leak queue */
	struct virtqueue *vq;
	/* The next epoch value the device should write through this leak queue */
	struct rand_epoch_data next_epoch;
};

struct virtrng_leak_queue;

struct virtrng_info {
	struct hwrng hwrng;
	struct virtqueue *vq;

	/* Leak queues */
	bool has_leakqs;
	struct virtrng_leak_queue leakq[2];
	int active_leakq;
	struct rng_epoch_notifier epoch_notifier;

	char name[25];
	int index;
	bool hwrng_register_done;
	bool hwrng_removed;
	/* data transfer */
	struct completion have_data;
	unsigned int data_avail;
	unsigned int data_idx;
	/* minimal size returned by rng_buffer_size() */
#if SMP_CACHE_BYTES < 32
	u8 data[32];
	u8 leak_data[32];
#else
	u8 data[SMP_CACHE_BYTES];
	u8 leak_data[SMP_CACHE_BYTES];
#endif
};

static struct virtrng_leak_queue *vq_to_leakq(struct virtrng_info *vi,
		struct virtqueue *vq)
{
	return &vi->leakq[vq->index - 1];
}

/*
 * Swaps the queues and returns the new active leak queue.
 * It assumes that the leak queues' lock is being held
 */
static void swap_leakqs(struct virtrng_info *vi)
{
	vi->active_leakq = 1 - vi->active_leakq;
}

static struct virtrng_leak_queue *get_active_leakq(struct virtrng_info *vi)
{
	return &vi->leakq[vi->active_leakq];
}

/*
 * Create the next epoch value that we will write through the leak queue.
 *
 * Subsequent epoch values will be written through alternate leak queues,
 * so the next epoch value for each queue will be:
 *
 * *-------------*----------------------------------------------*
 * | notifier_id | (current_epoch + 2) & RNG_EPOCH_COUNTER_MASK |
 * *-------------*----------------------------------------------*
 */
static void prepare_next_epoch(struct virtrng_info *vi, struct virtrng_leak_queue *leakq)
{
	leakq->next_epoch.data = ((leakq->next_epoch.data + 2) & RNG_EPOCH_COUNTER_MASK) |
		 (vi->epoch_notifier.id << RNG_EPOCH_ID_SHIFT);
}

static int do_fill_on_leak_request(struct virtrng_info *vi, struct virtqueue *vq, void *data,
				    size_t len)
{
	struct scatterlist sg;

	sg_init_one(&sg, data, len);
	return virtqueue_add_inbuf(vq, &sg, 1, data, GFP_KERNEL);
}

static int do_copy_on_leak_request(struct virtrng_info *vi, struct virtqueue *vq,
				   void *to, void *from, size_t len)
{
	struct scatterlist out, in, *sgs[2];

	sg_init_one(&out, from, len);
	sgs[0] = &out;
	sg_init_one(&in, to, len);
	sgs[1] = &in;

	return virtqueue_add_sgs(vq, sgs, 1, 1, to, GFP_KERNEL);
}

static int add_entropy_leak_requests(struct virtrng_info *vi, struct virtrng_leak_queue *leakq)
{
	do_fill_on_leak_request(vi, leakq->vq, &vi->leak_data, sizeof(vi->leak_data));
	/* Make sure the device writes the next valid epoch value */
	do_copy_on_leak_request(vi, leakq->vq, vi->epoch_notifier.epoch, &leakq->next_epoch,
			sizeof(u32));

	return 0;
}

static void entropy_leak_detected(struct virtqueue *vq)
{
	struct virtrng_info *vi = vq->vdev->priv;
	struct virtrng_leak_queue *activeq = get_active_leakq(vi);
	struct virtrng_leak_queue *leakq = vq_to_leakq(vi, vq);
	unsigned int len;
	void *buffer;

	/*
	 * The first time we see a used buffer in the active leak queue we swap queues
	 * so that new commands are added in the new active leak queue.
	 */
	if (vq == activeq->vq) {
		pr_info("%s: entropy leak detected!", vi->name);
		swap_leakqs(vi);
	}

	/* Drain all the used buffers from the queue */
	while ((buffer = virtqueue_get_buf(vq, &len)) != NULL) {
		if (buffer == vi->leak_data) {
			add_device_randomness(vi->leak_data, sizeof(vi->leak_data));

			/*
			 * Ensure we always have a pending request for random bytes on entropy
			 * leak. Do it here, after we have swapped leak queues, so it gets handled
			 * with the next entropy leak event.
			 */
			do_fill_on_leak_request(vi, vq, &vi->leak_data, sizeof(vi->leak_data));
		} else if (buffer == &vi->epoch_notifier.epoch->data) {
			/* Also, ensure we always have a pending request for bumping the epoch */
			prepare_next_epoch(vi, leakq);
			do_copy_on_leak_request(vi, vq, &vi->epoch_notifier.epoch->data,
					&leakq->next_epoch, sizeof(leakq->next_epoch));
		}
	}
}

static void random_recv_done(struct virtqueue *vq)
{
	struct virtrng_info *vi = vq->vdev->priv;
	unsigned int len;

	/* We can get spurious callbacks, e.g. shared IRQs + virtio_pci. */
	if (!virtqueue_get_buf(vi->vq, &len))
		return;

	smp_store_release(&vi->data_avail, len);
	complete(&vi->have_data);
}

static void request_entropy(struct virtrng_info *vi)
{
	struct scatterlist sg;

	reinit_completion(&vi->have_data);
	vi->data_idx = 0;

	sg_init_one(&sg, vi->data, sizeof(vi->data));

	/* There should always be room for one buffer. */
	virtqueue_add_inbuf(vi->vq, &sg, 1, vi->data, GFP_KERNEL);

	virtqueue_kick(vi->vq);
}

static unsigned int copy_data(struct virtrng_info *vi, void *buf,
			      unsigned int size)
{
	size = min_t(unsigned int, size, vi->data_avail);
	memcpy(buf, vi->data + vi->data_idx, size);
	vi->data_idx += size;
	vi->data_avail -= size;
	if (vi->data_avail == 0)
		request_entropy(vi);
	return size;
}

static int virtio_read(struct hwrng *rng, void *buf, size_t size, bool wait)
{
	int ret;
	struct virtrng_info *vi = (struct virtrng_info *)rng->priv;
	unsigned int chunk;
	size_t read;

	if (vi->hwrng_removed)
		return -ENODEV;

	read = 0;

	/* copy available data */
	if (smp_load_acquire(&vi->data_avail)) {
		chunk = copy_data(vi, buf, size);
		size -= chunk;
		read += chunk;
	}

	if (!wait)
		return read;

	/* We have already copied available entropy,
	 * so either size is 0 or data_avail is 0
	 */
	while (size != 0) {
		/* data_avail is 0 but a request is pending */
		ret = wait_for_completion_killable(&vi->have_data);
		if (ret < 0)
			return ret;
		/* if vi->data_avail is 0, we have been interrupted
		 * by a cleanup, but buffer stays in the queue
		 */
		if (vi->data_avail == 0)
			return read;

		chunk = copy_data(vi, buf + read, size);
		size -= chunk;
		read += chunk;
	}

	return read;
}

static void virtio_cleanup(struct hwrng *rng)
{
	struct virtrng_info *vi = (struct virtrng_info *)rng->priv;

	complete(&vi->have_data);
}

static int init_virtqueues(struct virtrng_info *vi, struct virtio_device *vdev)
{
	int ret, vqs_nr = 1;
	struct virtqueue *vqs[3];
	const char *names[3];
	vq_callback_t *callbacks[3];

	callbacks[0] = random_recv_done;
	names[0] = "input";

	if (vi->has_leakqs) {
		vqs_nr = 3;
		vi->active_leakq = 0;

		/* Register with random.c to get epoch info */
		ret = rng_register_epoch_notifier(&vi->epoch_notifier);
		if (ret)
			goto err_register_epoch;

		callbacks[1] = entropy_leak_detected;
		names[1] = "leakq.1";
		callbacks[2] = entropy_leak_detected;
		names[2] = "leakq.2";
	}

	ret = virtio_find_vqs(vdev, vqs_nr, vqs, callbacks, names, NULL);
	if (ret)
		goto err_find_vqs;

	vi->vq = vqs[0];
	if (vi->has_leakqs) {
		vi->leakq[0].vq = vqs[1];
		vi->leakq[0].next_epoch.data = 1;
		vi->leakq[1].vq = vqs[2];
		vi->leakq[1].next_epoch.data = 2;
	}

	return 0;

err_find_vqs:
	rng_unregister_epoch_notifier(&vi->epoch_notifier);
err_register_epoch:
	return ret;
}

static int probe_common(struct virtio_device *vdev)
{
	int err, index;
	struct virtrng_info *vi = NULL;

	vi = kzalloc(sizeof(struct virtrng_info), GFP_KERNEL);
	if (!vi)
		return -ENOMEM;

	vi->index = index = ida_simple_get(&rng_index_ida, 0, 0, GFP_KERNEL);
	if (index < 0) {
		err = index;
		goto err_ida;
	}
	sprintf(vi->name, "virtio_rng.%d", index);
	init_completion(&vi->have_data);

	vi->hwrng = (struct hwrng) {
		.read = virtio_read,
		.cleanup = virtio_cleanup,
		.priv = (unsigned long)vi,
		.name = vi->name,
	};
	vdev->priv = vi;

	vi->has_leakqs = virtio_has_feature(vdev, VIRTIO_RNG_F_LEAK);
	err = init_virtqueues(vi, vdev);
	if (err)
		goto err_find;

	virtio_device_ready(vdev);

	/* we always have a pending entropy request */
	request_entropy(vi);

	if (vi->has_leakqs) {
		/* we always have entropy-leak requests pending */
		add_entropy_leak_requests(vi, &vi->leakq[0]);
		add_entropy_leak_requests(vi, &vi->leakq[1]);
	}

	return 0;

err_find:
	ida_simple_remove(&rng_index_ida, index);
err_ida:
	kfree(vi);
	return err;
}

static void remove_common(struct virtio_device *vdev)
{
	struct virtrng_info *vi = vdev->priv;

	vi->hwrng_removed = true;
	vi->data_avail = 0;
	vi->data_idx = 0;
	complete(&vi->have_data);
	if (vi->hwrng_register_done)
		hwrng_unregister(&vi->hwrng);
	virtio_reset_device(vdev);
	vdev->config->del_vqs(vdev);
	ida_simple_remove(&rng_index_ida, vi->index);
	kfree(vi);
}

static int virtrng_probe(struct virtio_device *vdev)
{
	return probe_common(vdev);
}

static void virtrng_remove(struct virtio_device *vdev)
{
	remove_common(vdev);
}

static void virtrng_scan(struct virtio_device *vdev)
{
	struct virtrng_info *vi = vdev->priv;
	int err;

	err = hwrng_register(&vi->hwrng);
	if (!err)
		vi->hwrng_register_done = true;
}

#ifdef CONFIG_PM_SLEEP
static int virtrng_freeze(struct virtio_device *vdev)
{
	remove_common(vdev);
	return 0;
}

static int virtrng_restore(struct virtio_device *vdev)
{
	int err;

	err = probe_common(vdev);
	if (!err) {
		struct virtrng_info *vi = vdev->priv;

		/*
		 * Set hwrng_removed to ensure that virtio_read()
		 * does not block waiting for data before the
		 * registration is complete.
		 */
		vi->hwrng_removed = true;
		err = hwrng_register(&vi->hwrng);
		if (!err) {
			vi->hwrng_register_done = true;
			vi->hwrng_removed = false;
		}
	}

	return err;
}
#endif

static const struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_RNG, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static unsigned int features[] = {
	VIRTIO_RNG_F_LEAK,
};

static struct virtio_driver virtio_rng_driver = {
	.feature_table = features,
	.feature_table_size = ARRAY_SIZE(features),
	.driver.name =	KBUILD_MODNAME,
	.driver.owner =	THIS_MODULE,
	.id_table =	id_table,
	.probe =	virtrng_probe,
	.remove =	virtrng_remove,
	.scan =		virtrng_scan,
#ifdef CONFIG_PM_SLEEP
	.freeze =	virtrng_freeze,
	.restore =	virtrng_restore,
#endif
};

module_virtio_driver(virtio_rng_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio random number driver");
MODULE_LICENSE("GPL");
