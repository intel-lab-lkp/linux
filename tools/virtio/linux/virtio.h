/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_VIRTIO_H
#define LINUX_VIRTIO_H
#include <linux/scatterlist.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/dma-mapping.h>

struct device {
	void *parent;
};

union virtio_map {
	struct device *dma_dev;
};

struct virtio_device {
	struct device dev;
	u64 features;
	struct list_head vqs;
	spinlock_t vqs_list_lock;
	const struct virtio_config_ops *config;
	const struct virtio_map_ops *map;
	union virtio_map vmap;
};

struct virtqueue {
	struct list_head list;
	void (*callback)(struct virtqueue *vq);
	const char *name;
	struct virtio_device *vdev;
        unsigned int index;
        unsigned int num_free;
	unsigned int num_max;
	void *priv;
	bool reset;
};

/* Interfaces exported by virtio_ring. */
int virtqueue_add_sgs(struct virtqueue *vq,
		      struct scatterlist *sgs[],
		      unsigned int out_sgs,
		      unsigned int in_sgs,
		      void *data,
		      gfp_t gfp);

int virtqueue_add_outbuf(struct virtqueue *vq,
			 struct scatterlist sg[], unsigned int num,
			 void *data,
			 gfp_t gfp);

int virtqueue_add_inbuf(struct virtqueue *vq,
			struct scatterlist sg[], unsigned int num,
			void *data,
			gfp_t gfp);

bool virtqueue_kick(struct virtqueue *vq);

void *virtqueue_get_buf(struct virtqueue *vq, unsigned int *len);

void virtqueue_disable_cb(struct virtqueue *vq);

bool virtqueue_enable_cb(struct virtqueue *vq);
bool virtqueue_enable_cb_delayed(struct virtqueue *vq);

void *virtqueue_detach_unused_buf(struct virtqueue *vq);
struct virtqueue *vring_new_virtqueue(unsigned int index,
				      unsigned int num,
				      unsigned int vring_align,
				      struct virtio_device *vdev,
				      bool weak_barriers,
				      bool ctx,
				      void *pages,
				      bool (*notify)(struct virtqueue *vq),
				      void (*callback)(struct virtqueue *vq),
				      const char *name);
void vring_del_virtqueue(struct virtqueue *vq);

void *virtqueue_map_alloc_coherent(struct virtio_device *vdev,
				   union virtio_map mapping_token,
				   size_t size, dma_addr_t *dma_handle,
				   gfp_t gfp);

void virtqueue_map_free_coherent(struct virtio_device *vdev,
				 union virtio_map mapping_token,
				 size_t size, void *vaddr,
				 dma_addr_t dma_handle);

dma_addr_t virtqueue_map_page_attrs(const struct virtqueue *_vq,
				    struct page *page,
				    unsigned long offset,
				    size_t size,
				    enum dma_data_direction dir,
				    unsigned long attrs);

void virtqueue_unmap_page_attrs(const struct virtqueue *_vq,
				dma_addr_t dma_handle,
				size_t size, enum dma_data_direction dir,
				unsigned long attrs);
#endif
