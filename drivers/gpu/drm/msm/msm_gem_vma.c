// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2016 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#include "msm_drv.h"
#include "msm_gem.h"
#include "msm_mmu.h"

/* SDE address space operations */
static void smmu_aspace_unmap_vma(struct msm_gem_address_space *aspace,
		struct msm_gem_vma *vma, struct sg_table *sgt,
		unsigned int flags)
{
	if (!vma->iova)
		return;

	if (aspace) {
		aspace->mmu->funcs->unmap_dma_buf(aspace->mmu, sgt,
				DMA_BIDIRECTIONAL, flags);
	}

	vma->iova = 0;
	msm_gem_address_space_put(aspace);
}

static int smmu_aspace_map_vma(struct msm_gem_address_space *aspace,
		struct msm_gem_vma *vma, struct sg_table *sgt,
		int npages, unsigned int flags)
{
	int ret = -EINVAL;

	if (!aspace || !aspace->domain_attached)
		return ret;

	ret = aspace->mmu->funcs->map_dma_buf(aspace->mmu, sgt,
			DMA_BIDIRECTIONAL, flags);
	if (!ret)
		vma->iova = sg_dma_address(sgt->sgl);

	/* Get a reference to the aspace to keep it around */
	kref_get(&aspace->kref);

	return ret;
}

static void smmu_aspace_destroy(struct msm_gem_address_space *aspace)
{
	if (aspace->mmu)
		aspace->mmu->funcs->destroy(aspace->mmu);
	put_pid(aspace->pid);
	kfree(aspace);
}

static void smmu_aspace_add_to_active(
		struct msm_gem_address_space *aspace,
		struct msm_gem_object *msm_obj)
{
	WARN_ON(!mutex_is_locked(&aspace->list_lock));
	list_move_tail(&msm_obj->iova_list, &aspace->active_list);
	msm_obj->in_active_list = true;
}

struct msm_gem_address_space *
msm_gem_address_space_get(struct msm_gem_address_space *aspace)
{
	if (!IS_ERR_OR_NULL(aspace))
		kref_get(&aspace->kref);

	return aspace;
}

/* Actually unmap memory for the vma */
void msm_gem_purge_vma(struct msm_gem_address_space *aspace,
		struct msm_gem_vma *vma)
{
	unsigned size = vma->node.size << PAGE_SHIFT;

	/* Print a message if we try to purge a vma in use */
	if (WARN_ON(vma->inuse > 0))
		return;

	/* Don't do anything if the memory isn't mapped */
	if (!vma->mapped)
		return;

	if (aspace->mmu)
		aspace->mmu->funcs->unmap(aspace->mmu, vma->iova, size);

	vma->mapped = false;
}

/* Remove reference counts for the mapping */
void msm_gem_unmap_vma(struct msm_gem_address_space *aspace,
		struct msm_gem_vma *vma)
{
	if (!WARN_ON(!vma->iova))
		vma->inuse--;
}

int
msm_gem_map_vma(struct msm_gem_address_space *aspace,
		struct msm_gem_vma *vma, int prot,
		struct sg_table *sgt, int npages)
{
	unsigned size = npages << PAGE_SHIFT;
	int ret = 0;

	if (WARN_ON(!vma->iova))
		return -EINVAL;

	/* Increase the usage counter */
	vma->inuse++;

	if (vma->mapped)
		return 0;

	vma->mapped = true;

	if (aspace && aspace->mmu)
		ret = aspace->mmu->funcs->map(aspace->mmu, vma->iova, sgt,
				size, prot);

	if (ret) {
		vma->mapped = false;
		vma->inuse--;
	}

	return ret;
}

/* Close an iova.  Warn if it is still in use */
void msm_gem_close_vma(struct msm_gem_address_space *aspace,
		struct msm_gem_vma *vma)
{
	if (WARN_ON(vma->inuse > 0 || vma->mapped))
		return;

	spin_lock(&aspace->lock);
	if (vma->iova)
		drm_mm_remove_node(&vma->node);
	spin_unlock(&aspace->lock);

	vma->iova = 0;

	msm_gem_address_space_put(aspace);
}

/* Initialize a new vma and allocate an iova for it */
int msm_gem_init_vma(struct msm_gem_address_space *aspace,
		struct msm_gem_vma *vma, int npages,
		u64 range_start, u64 range_end)
{
	int ret;

	if (WARN_ON(vma->iova))
		return -EBUSY;

	spin_lock(&aspace->lock);
	ret = drm_mm_insert_node_in_range(&aspace->mm, &vma->node, npages, 0,
		0, range_start, range_end, 0);
	spin_unlock(&aspace->lock);

	if (ret)
		return ret;

	vma->iova = vma->node.start << PAGE_SHIFT;
	vma->mapped = false;

	kref_get(&aspace->kref);

	return 0;
}

static void iommu_aspace_destroy(struct msm_gem_address_space *aspace)
{
	drm_mm_takedown(&aspace->mm);
	if (aspace->mmu)
		aspace->mmu->funcs->destroy(aspace->mmu);
}

static const struct msm_gem_aspace_ops msm_iommu_aspace_ops = {
	.map = iommu_aspace_map_vma,
	.unmap = iommu_aspace_unmap_vma,
	.destroy = iommu_aspace_destroy,
};

struct msm_gem_address_space *
msm_gem_address_space_create(struct msm_mmu *mmu, const char *name,
		u64 va_start, u64 size)
{
	struct msm_gem_address_space *aspace;

	if (IS_ERR(mmu))
		return ERR_CAST(mmu);

	aspace = kzalloc(sizeof(*aspace), GFP_KERNEL);
	if (!aspace)
		return ERR_PTR(-ENOMEM);

	spin_lock_init(&aspace->lock);
	aspace->name = name;
	aspace->mmu = mmu;

	drm_mm_init(&aspace->mm, va_start >> PAGE_SHIFT, size >> PAGE_SHIFT);

	kref_init(&aspace->kref);

	return aspace;
}


/* Generic address space operations */
static void
msm_gem_address_space_destroy(struct kref *kref)
{
	struct msm_gem_address_space *aspace = container_of(kref,
			struct msm_gem_address_space, kref);

	if (aspace && aspace->ops->destroy)
		aspace->ops->destroy(aspace);

	kfree(aspace);
}

void msm_gem_address_space_put(struct msm_gem_address_space *aspace)
{
	if (aspace)
		kref_put(&aspace->kref, msm_gem_address_space_destroy);
}

void
msm_gem_unmap_vma(struct msm_gem_address_space *aspace,
		struct msm_gem_vma *vma, struct sg_table *sgt,
		unsigned int flags)
{
	if (aspace && aspace->ops->unmap)
		aspace->ops->unmap(aspace, vma, sgt, flags);
}

int
msm_gem_map_vma(struct msm_gem_address_space *aspace,
		struct msm_gem_vma *vma, struct sg_table *sgt, int npages,
		unsigned int flags)
{
	if (aspace && aspace->ops->map)
		return aspace->ops->map(aspace, vma, sgt, npages, flags);

	return -EINVAL;
}

struct device *msm_gem_get_aspace_device(struct msm_gem_address_space *aspace)
{
	struct device *client_dev = NULL;

	if (aspace && aspace->mmu && aspace->mmu->funcs->get_dev)
		client_dev = aspace->mmu->funcs->get_dev(aspace->mmu);

	return client_dev;
}

void msm_gem_add_obj_to_aspace_active_list(
		struct msm_gem_address_space *aspace,
		struct drm_gem_object *obj)
{
	struct msm_gem_object *msm_obj = to_msm_bo(obj);

	if (aspace && aspace->ops && aspace->ops->add_to_active)
		aspace->ops->add_to_active(aspace, msm_obj);
}

void msm_gem_remove_obj_from_aspace_active_list(
		struct msm_gem_address_space *aspace,
		struct drm_gem_object *obj)
{
	struct msm_gem_object *msm_obj = to_msm_bo(obj);

	if (aspace && aspace->ops && aspace->ops->remove_from_active)
		aspace->ops->remove_from_active(aspace, msm_obj);
}

int msm_gem_address_space_register_cb(struct msm_gem_address_space *aspace,
		void (*cb)(void *, bool),
		void *cb_data)
{
	if (aspace && aspace->ops && aspace->ops->register_cb)
		return aspace->ops->register_cb(aspace, cb, cb_data);

	return -EINVAL;
}

int msm_gem_address_space_unregister_cb(struct msm_gem_address_space *aspace,
		void (*cb)(void *, bool),
		void *cb_data)
{
	if (aspace && aspace->ops && aspace->ops->unregister_cb)
		return aspace->ops->unregister_cb(aspace, cb, cb_data);

	return -EINVAL;
}

