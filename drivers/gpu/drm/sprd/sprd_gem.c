// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Unisoc Inc.
 * Copyright (C) 2025 Otto Pflüger
 */

#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/vmalloc.h>

#include <drm/drm_gem_dma_helper.h>

#include "sprd_drm.h"
#include "sprd_gem.h"

static int sprd_gem_get_pages(struct sprd_gem_obj *sprd_gem)
{
	struct drm_gem_object *obj = &sprd_gem->base;
	struct scatterlist *s;
	int i, ret;

	sprd_gem->pages = drm_gem_get_pages(obj);
	if (IS_ERR(sprd_gem->pages))
		return PTR_ERR(sprd_gem->pages);

	sprd_gem->sgt = drm_prime_pages_to_sg(obj->dev, sprd_gem->pages,
					      obj->size >> PAGE_SHIFT);
	if (IS_ERR(sprd_gem->sgt)) {
		ret = PTR_ERR(sprd_gem->sgt);
		goto err_put_pages;
	}

	/*
	 * From drivers/gpu/drm/rockchip/rockchip_drm_gem.c:
	 *
	 * Fake up the SG table so that dma_sync_sg_for_device() can be used
	 * to flush the pages associated with it.
	 *
	 * TODO: Replace this by drm_clflush_sg() once it can be implemented
	 * without relying on symbols that are not exported.
	 */
	for_each_sgtable_sg(sprd_gem->sgt, s, i)
		sg_dma_address(s) = sg_phys(s);

	dma_sync_sgtable_for_device(obj->dev->dev, sprd_gem->sgt, DMA_TO_DEVICE);

	return 0;

err_put_pages:
	drm_gem_put_pages(obj, sprd_gem->pages, false, false);
	return ret;
}

static void sprd_gem_put_pages(struct sprd_gem_obj *sprd_gem)
{
	sg_free_table(sprd_gem->sgt);
	kfree(sprd_gem->sgt);
	drm_gem_put_pages(&sprd_gem->base, sprd_gem->pages, true, true);
}

static int sprd_gem_iommu_map(struct sprd_gem_obj *sprd_gem)
{
	struct drm_gem_object *obj = &sprd_gem->base;
	struct sprd_drm *sprd = to_sprd_drm(obj->dev);
	ssize_t ret;

	mutex_lock(&sprd->mm_lock);
	ret = drm_mm_insert_node_generic(&sprd->mm, &sprd_gem->mm,
					 obj->size, PAGE_SIZE, 0, 0);
	mutex_unlock(&sprd->mm_lock);

	if (ret) {
		drm_err(obj->dev, "out of I/O virtual memory\n");
		return ret;
	}

	sprd_gem->dma_addr = sprd_gem->mm.start;

	ret = iommu_map_sgtable(sprd->iommu_domain, sprd_gem->dma_addr,
				sprd_gem->sgt, IOMMU_READ | IOMMU_WRITE);
	if (ret < (ssize_t)obj->size) {
		drm_err(obj->dev,
			"failed to map buffer: requested=%zd, ret=%zd\n",
			obj->size, ret);
		ret = -ENOMEM;
		goto err_remove_node;
	}

	sprd_gem->map_size = ret;

	return 0;

err_remove_node:
	mutex_lock(&sprd->mm_lock);
	drm_mm_remove_node(&sprd_gem->mm);
	mutex_unlock(&sprd->mm_lock);
	return ret;
}

static void sprd_gem_iommu_unmap(struct sprd_gem_obj *sprd_gem)
{
	struct drm_gem_object *obj = &sprd_gem->base;
	struct sprd_drm *sprd = to_sprd_drm(obj->dev);

	iommu_unmap(sprd->iommu_domain, sprd_gem->dma_addr, sprd_gem->map_size);

	mutex_lock(&sprd->mm_lock);
	drm_mm_remove_node(&sprd_gem->mm);
	mutex_unlock(&sprd->mm_lock);
}

static void sprd_gem_free_object(struct drm_gem_object *obj)
{
	struct sprd_drm *sprd = to_sprd_drm(obj->dev);
	struct sprd_gem_obj *sprd_gem = to_sprd_gem_obj(obj);

	if (obj->import_attach) {
		if (sprd->iommu_domain)
			sprd_gem_iommu_unmap(sprd_gem);
		drm_prime_gem_destroy(obj, sprd_gem->sgt);
	} else {
		if (sprd->iommu_domain) {
			sprd_gem_iommu_unmap(sprd_gem);
			sprd_gem_put_pages(sprd_gem);
		} else {
			dma_free_wc(obj->dev->dev, obj->size,
				    sprd_gem->vaddr, sprd_gem->dma_addr);
		}
	}

	drm_gem_object_release(obj);

	kfree(sprd_gem);
}

static struct sg_table *sprd_gem_object_get_sg_table(struct drm_gem_object *obj)
{
	struct sprd_gem_obj *sprd_gem = to_sprd_gem_obj(obj);
	struct sg_table *sgt;
	int ret;

	if (sprd_gem->pages)
		return drm_prime_pages_to_sg(obj->dev, sprd_gem->pages,
					     obj->size >> PAGE_SHIFT);

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	ret = dma_get_sgtable(obj->dev->dev, sgt, sprd_gem->vaddr,
			      sprd_gem->dma_addr, obj->size);
	if (ret < 0) {
		kfree(sgt);
		return ERR_PTR(ret);
	}

	return sgt;
}

static int sprd_gem_object_vmap(struct drm_gem_object *obj,
				struct iosys_map *map)
{
	struct sprd_gem_obj *sprd_gem = to_sprd_gem_obj(obj);
	void *vaddr;

	if (!sprd_gem->pages) {
		iosys_map_set_vaddr(map, sprd_gem->vaddr);
		return 0;
	}

	vaddr = vmap(sprd_gem->pages, sprd_gem->base.size >> PAGE_SHIFT,
		     VM_MAP, pgprot_writecombine(PAGE_KERNEL));
	if (!vaddr)
		return -ENOMEM;

	iosys_map_set_vaddr(map, vaddr);

	return 0;
}

static void sprd_gem_object_vunmap(struct drm_gem_object *obj,
				   struct iosys_map *map)
{
	struct sprd_gem_obj *sprd_gem = to_sprd_gem_obj(obj);

	if (sprd_gem->pages)
		vunmap(map->vaddr);
}

static int sprd_gem_object_mmap(struct drm_gem_object *obj,
				struct vm_area_struct *vma)
{
	struct sprd_gem_obj *sprd_gem = to_sprd_gem_obj(obj);
	int ret;

	/*
	 * Clear the VM_PFNMAP flag that was set by drm_gem_mmap(), and set the
	 * vm_pgoff (used as a fake buffer offset by DRM) to 0 as we want to map
	 * the whole buffer.
	 */
	vm_flags_mod(vma, VM_DONTEXPAND, VM_PFNMAP);
	vma->vm_pgoff = 0;

	vma->vm_page_prot = pgprot_writecombine(vm_get_page_prot(vma->vm_flags));

	if (sprd_gem->pages)
		ret = vm_map_pages(vma, sprd_gem->pages,
				   obj->size >> PAGE_SHIFT);
	else
		ret = dma_mmap_wc(obj->dev->dev, vma, sprd_gem->vaddr,
				  sprd_gem->dma_addr, obj->size);

	if (ret)
		drm_gem_vm_close(vma);

	return ret;
}

static const struct drm_gem_object_funcs sprd_gem_object_funcs = {
	.free = sprd_gem_free_object,
	.get_sg_table = sprd_gem_object_get_sg_table,
	.vmap = sprd_gem_object_vmap,
	.vunmap = sprd_gem_object_vunmap,
	.mmap = sprd_gem_object_mmap,
	.vm_ops = &drm_gem_dma_vm_ops,
};

static struct sprd_gem_obj *sprd_gem_obj_create(struct drm_device *drm,
						unsigned long size)
{
	struct sprd_gem_obj *sprd_gem;
	int ret;

	sprd_gem = kzalloc(sizeof(*sprd_gem), GFP_KERNEL);
	if (!sprd_gem)
		return ERR_PTR(-ENOMEM);

	sprd_gem->base.funcs = &sprd_gem_object_funcs;

	ret = drm_gem_object_init(drm, &sprd_gem->base, size);
	if (ret < 0) {
		drm_err(drm, "failed to initialize gem object\n");
		goto err_free;
	}

	return sprd_gem;

err_free:
	kfree(sprd_gem);
	return ERR_PTR(ret);
}

int sprd_gem_dumb_create(struct drm_file *file_priv, struct drm_device *drm,
			 struct drm_mode_create_dumb *args)
{
	struct sprd_drm *sprd = to_sprd_drm(drm);
	struct sprd_gem_obj *sprd_gem;
	int ret;

	args->pitch = DIV_ROUND_UP(args->width * args->bpp, 8);
	args->size = round_up(args->pitch * args->height, PAGE_SIZE);

	sprd_gem = sprd_gem_obj_create(drm, args->size);
	if (IS_ERR(sprd_gem))
		return PTR_ERR(sprd_gem);

	if (sprd->iommu_domain) {
		ret = sprd_gem_get_pages(sprd_gem);
		if (ret < 0)
			goto err_free_object;

		ret = sprd_gem_iommu_map(sprd_gem);
		if (ret < 0) {
			sprd_gem_put_pages(sprd_gem);
			goto err_free_object;
		}
	} else {
		sprd_gem->vaddr = dma_alloc_wc(drm->dev, args->size,
					       &sprd_gem->dma_addr,
					       GFP_KERNEL);
		if (!sprd_gem->vaddr) {
			drm_err(drm, "failed to allocate buffer of size %llu\n",
				args->size);
			ret = -ENOMEM;
			goto err_free_object;
		}
	}

	ret = drm_gem_handle_create(file_priv, &sprd_gem->base, &args->handle);
	if (ret)
		goto err_free_buf;

	/* The handle holds its own reference, so we can drop ours now. */
	drm_gem_object_put(&sprd_gem->base);

	return 0;

err_free_buf:
	if (sprd->iommu_domain) {
		sprd_gem_iommu_unmap(sprd_gem);
		sprd_gem_put_pages(sprd_gem);
	} else {
		dma_free_wc(drm->dev, args->size, sprd_gem->vaddr,
			    sprd_gem->dma_addr);
	}
err_free_object:
	drm_gem_object_release(&sprd_gem->base);
	kfree(sprd_gem);
	return ret;
}

struct drm_gem_object *
sprd_gem_prime_import_sg_table(struct drm_device *drm,
			       struct dma_buf_attachment *attach,
			       struct sg_table *sgt)
{
	struct sprd_drm *sprd = to_sprd_drm(drm);
	struct sprd_gem_obj *sprd_gem;
	int ret;

	sprd_gem = sprd_gem_obj_create(drm, attach->dmabuf->size);
	if (IS_ERR(sprd_gem))
		return ERR_CAST(sprd_gem);

	sprd_gem->sgt = sgt;

	if (sprd->iommu_domain) {
		ret = sprd_gem_iommu_map(sprd_gem);
		if (ret < 0) {
			drm_err(drm, "failed to import with IOMMU: %d\n", ret);
			goto err_free_object;
		}
	} else {
		sprd_gem->dma_addr = sg_dma_address(sgt->sgl);
	}

	return &sprd_gem->base;

err_free_object:
	drm_gem_object_release(&sprd_gem->base);
	kfree(sprd_gem);
	return ERR_PTR(ret);
}
