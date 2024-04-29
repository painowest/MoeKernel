// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 ARM Ltd.
 * Author: Catalin Marinas <catalin.marinas@arm.com>
 */

#include <linux/gfp.h>
#include <linux/cache.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-iommu.h>
#include <xen/xen.h>
#include <xen/swiotlb-xen.h>
#include <trace/hooks/iommu.h>

#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include <asm/dma-iommu.h>
#include <linux/dma-mapping-fast.h>
#include <linux/msm_dma_iommu_mapping.h>
#include <linux/arm-smmu-errata.h>
#include <soc/qcom/secure_buffer.h>


void arch_sync_dma_for_device(phys_addr_t paddr, size_t size,
		enum dma_data_direction dir)
{
	__dma_map_area(phys_to_virt(paddr), size, dir);
}

void arch_sync_dma_for_cpu(phys_addr_t paddr, size_t size,
		enum dma_data_direction dir)
{
	__dma_unmap_area(phys_to_virt(paddr), size, dir);
}

void arch_dma_prep_coherent(struct page *page, size_t size)
{
	__dma_flush_area(page_address(page), size);
}

#ifdef CONFIG_IOMMU_DMA
void arch_teardown_dma_ops(struct device *dev)
{
	dev->dma_ops = NULL;
}
#endif

void arch_setup_dma_ops(struct device *dev, u64 dma_base, u64 size,
			const struct iommu_ops *iommu, bool coherent)
{
	int cls = cache_line_size_of_cpu();

	WARN_TAINT(!coherent && cls > ARCH_DMA_MINALIGN,
		   TAINT_CPU_OUT_OF_SPEC,
		   "%s %s: ARCH_DMA_MINALIGN smaller than CTR_EL0.CWG (%d < %d)",
		   dev_driver_string(dev), dev_name(dev),
		   ARCH_DMA_MINALIGN, cls);

	dev->dma_coherent = coherent;
	if (iommu) {
		iommu_setup_dma_ops(dev, dma_base, dma_base + size - 1);
		trace_android_rvh_iommu_setup_dma_ops(dev, dma_base, dma_base + size - 1);
	}

#ifdef CONFIG_XEN
	if (xen_swiotlb_detect())
		dev->dma_ops = &xen_swiotlb_dma_ops;
#endif
}
EXPORT_SYMBOL(arch_setup_dma_ops);

#ifdef CONFIG_ARM64_DMA_USE_IOMMU

static int __get_iommu_pgprot(unsigned long attrs, int prot,
			      bool coherent)
{
	if (!(attrs & DMA_ATTR_EXEC_MAPPING))
		prot |= IOMMU_NOEXEC;
	if (attrs & DMA_ATTR_IOMMU_USE_UPSTREAM_HINT)
		prot |= IOMMU_USE_UPSTREAM_HINT;
	if (attrs & DMA_ATTR_IOMMU_USE_LLC_NWA)
		prot |= IOMMU_USE_LLC_NWA;
	if (coherent)
		prot |= IOMMU_CACHE;

	return prot;
}

/*
 * Make an area consistent for devices.
 * Note: Drivers should NOT use this function directly, as it will break
 * platforms with CONFIG_DMABOUNCE.
 * Use the driver DMA support - see dma-mapping.h (dma_sync_*)
 */
static void __dma_page_cpu_to_dev(struct page *page, unsigned long off,
	size_t size, enum dma_data_direction dir)
{
	__dma_map_area(page_address(page) + off, size, dir);
}

static void __dma_page_dev_to_cpu(struct page *page, unsigned long off,
	size_t size, enum dma_data_direction dir)
{
	__dma_unmap_area(page_address(page) + off, size, dir);

	/*
	 * Mark the D-cache clean for this page to avoid extra flushing.
	 */
	if (dir != DMA_TO_DEVICE && off == 0 && size >= PAGE_SIZE)
		set_bit(PG_dcache_clean, &page->flags);
}

/* IOMMU */

static void __dma_clear_buffer(struct page *page, size_t size,
			       unsigned long attrs, bool is_coherent)
{
	/*
	 * Ensure that the allocated pages are zeroed, and that any data
	 * lurking in the kernel direct-mapped region is invalidated.
	 */
	void *ptr = page_address(page);

	if (!(attrs & DMA_ATTR_SKIP_ZEROING))
		memset(ptr, 0, size);
	if (!is_coherent)
		__dma_flush_area(ptr, size);
}

static inline dma_addr_t __alloc_iova(struct dma_iommu_mapping *mapping,
				      size_t size)
{
	unsigned int order;
	unsigned int align = 0;
	unsigned int count, start;
	unsigned long flags;
	dma_addr_t iova;
	size_t guard_len;

	size = PAGE_ALIGN(size);
	if (mapping->min_iova_align)
		guard_len = ALIGN(size, mapping->min_iova_align) - size;
	else
		guard_len = 0;

	order = get_order(size + guard_len);
	if (order > CONFIG_ARM64_DMA_IOMMU_ALIGNMENT)
		order = CONFIG_ARM64_DMA_IOMMU_ALIGNMENT;

	count = PAGE_ALIGN(size + guard_len) >> PAGE_SHIFT;
	align = (1 << order) - 1;

	spin_lock_irqsave(&mapping->lock, flags);
	start = bitmap_find_next_zero_area(mapping->bitmap, mapping->bits, 0,
					   count, align);
	if (start > mapping->bits) {
		spin_unlock_irqrestore(&mapping->lock, flags);
		return DMA_ERROR_CODE;
	}

	bitmap_set(mapping->bitmap, start, count);
	spin_unlock_irqrestore(&mapping->lock, flags);

	iova = mapping->base + (start << PAGE_SHIFT);

	if (guard_len &&
		iommu_map(mapping->domain, iova + size,
			page_to_phys(mapping->guard_page),
			guard_len, ARM_SMMU_GUARD_PROT)) {

		spin_lock_irqsave(&mapping->lock, flags);
		bitmap_clear(mapping->bitmap, start, count);
		spin_unlock_irqrestore(&mapping->lock, flags);
		return DMA_ERROR_CODE;
	}

	return iova;
}

static inline void __free_iova(struct dma_iommu_mapping *mapping,
			       dma_addr_t addr, size_t size)
{
	unsigned int start;
	unsigned int count;
	unsigned long flags;
	size_t guard_len;

	addr = addr & PAGE_MASK;
	size = PAGE_ALIGN(size);
	if (mapping->min_iova_align) {
		guard_len = ALIGN(size, mapping->min_iova_align) - size;
		iommu_unmap(mapping->domain, addr + size, guard_len);
	} else {
		guard_len = 0;
	}

	start = (addr - mapping->base) >> PAGE_SHIFT;
	count = (size + guard_len) >> PAGE_SHIFT;
	spin_lock_irqsave(&mapping->lock, flags);
	bitmap_clear(mapping->bitmap, start, count);
	spin_unlock_irqrestore(&mapping->lock, flags);
}

static struct page **__iommu_alloc_buffer(struct device *dev, size_t size,
					  gfp_t gfp, unsigned long attrs)
{
	struct page **pages;
	size_t count = size >> PAGE_SHIFT;
	size_t array_size = count * sizeof(struct page *);
	int i = 0;
	bool is_coherent = is_dma_coherent(dev, attrs);
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;
	unsigned int alloc_sizes = mapping->domain->pgsize_bitmap;
	unsigned long order_mask;

	if (array_size <= PAGE_SIZE)
		pages = kzalloc(array_size, gfp);
	else
		pages = vzalloc(array_size);
	if (!pages)
		return NULL;

	if (attrs & DMA_ATTR_FORCE_CONTIGUOUS) {
		unsigned long order = get_order(size);
		struct page *page;

		page = dma_alloc_from_contiguous(dev, count, order, GFP_KERNEL);
		if (!page)
			goto error;

		__dma_clear_buffer(page, size, attrs, is_coherent);

		for (i = 0; i < count; i++)
			pages[i] = page + i;

		return pages;
	}

	/*
	 * IOMMU can map any pages, so himem can also be used here
	 */
	gfp |= __GFP_NOWARN | __GFP_HIGHMEM;
	order_mask = alloc_sizes >> PAGE_SHIFT;
	order_mask &= (2U << MAX_ORDER) - 1;
	if (!order_mask)
		goto error;

	while (count) {
		int j, order;

		order_mask &= (2U << __fls(count)) - 1;
		order = __fls(order_mask);

		pages[i] = alloc_pages(order ? (gfp | __GFP_NORETRY) &
					~__GFP_RECLAIM : gfp, order);
		while (!pages[i] && order) {
			order_mask &= ~(1U << order);
			order = __fls(order_mask);
			pages[i] = alloc_pages(order ? (gfp | __GFP_NORETRY) &
					~__GFP_RECLAIM : gfp, order);
		}

		if (!pages[i])
			goto error;

		if (order) {
			split_page(pages[i], order);
			j = 1 << order;
			while (--j)
				pages[i + j] = pages[i] + j;
		}

		__dma_clear_buffer(pages[i], PAGE_SIZE << order, attrs,
				   is_coherent);
		i += 1 << order;
		count -= 1 << order;
	}

	return pages;
error:
	while (i--)
		if (pages[i])
			__free_pages(pages[i], 0);
	if (array_size <= PAGE_SIZE)
		kfree(pages);
	else
		vfree(pages);
	return NULL;
}

static int __iommu_free_buffer(struct device *dev, struct page **pages,
			       size_t size, unsigned long attrs)
{
	int count = size >> PAGE_SHIFT;
	int array_size = count * sizeof(struct page *);
	int i;

	if (attrs & DMA_ATTR_FORCE_CONTIGUOUS) {
		dma_release_from_contiguous(dev, pages[0], count);
	} else {
		for (i = 0; i < count; i++)
			if (pages[i])
				__free_pages(pages[i], 0);
	}

	if (array_size <= PAGE_SIZE)
		kfree(pages);
	else
		vfree(pages);
	return 0;
}

/*
 * Create a CPU mapping for a specified pages
 */
static void *
__iommu_alloc_remap(struct page **pages, size_t size, gfp_t gfp, pgprot_t prot,
		    const void *caller)
{
	return dma_common_pages_remap(pages, size, VM_USERMAP, prot, caller);
}

/*
 * Create a mapping in device IO address space for specified pages
 */
static dma_addr_t __iommu_create_mapping(struct device *dev,
					struct page **pages, size_t size,
					unsigned long attrs)
{
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;
	unsigned int count = PAGE_ALIGN(size) >> PAGE_SHIFT;
	dma_addr_t dma_addr, iova;
	int i, ret;
	int prot = IOMMU_READ | IOMMU_WRITE;

	dma_addr = __alloc_iova(mapping, size);
	if (dma_addr == DMA_ERROR_CODE)
		return dma_addr;

	prot = __get_iommu_pgprot(attrs, prot,
				  is_dma_coherent(dev, attrs));

	iova = dma_addr;
	for (i = 0; i < count; ) {
		unsigned int next_pfn = page_to_pfn(pages[i]) + 1;
		phys_addr_t phys = page_to_phys(pages[i]);
		unsigned int len, j;

		for (j = i + 1; j < count; j++, next_pfn++)
			if (page_to_pfn(pages[j]) != next_pfn)
				break;

		len = (j - i) << PAGE_SHIFT;
		ret = iommu_map(mapping->domain, iova, phys, len, prot);
		if (ret < 0)
			goto fail;
		iova += len;
		i = j;
	}
	return dma_addr;
fail:
	iommu_unmap(mapping->domain, dma_addr, iova-dma_addr);
	__free_iova(mapping, dma_addr, size);
	return DMA_ERROR_CODE;
}

static int __iommu_remove_mapping(struct device *dev, dma_addr_t iova,
				size_t size)
{
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;

	/*
	 * add optional in-page offset from iova to size and align
	 * result to page size
	 */
	size = PAGE_ALIGN((iova & ~PAGE_MASK) + size);
	iova &= PAGE_MASK;

	iommu_unmap(mapping->domain, iova, size);
	__free_iova(mapping, iova, size);
	return 0;
}

static struct page **__atomic_get_pages(void *addr)
{
	struct page *page;
	phys_addr_t phys;

	phys = gen_pool_virt_to_phys(atomic_pool, (unsigned long)addr);
	page = phys_to_page(phys);

	return (struct page **)page;
}

static struct page **__iommu_get_pages(void *cpu_addr, unsigned long attrs)
{
	struct vm_struct *area;

	if (__in_atomic_pool(cpu_addr, PAGE_SIZE))
		return __atomic_get_pages(cpu_addr);

	if (attrs & DMA_ATTR_NO_KERNEL_MAPPING)
		return cpu_addr;

	area = find_vm_area(cpu_addr);
	if (area)
		return area->pages;
	return NULL;
}

static void *__iommu_alloc_atomic(struct device *dev, size_t size,
				  dma_addr_t *handle, gfp_t gfp,
				  unsigned long attrs)
{
	struct page *page;
	struct page **pages;
	size_t count = size >> PAGE_SHIFT;
	size_t array_size = count * sizeof(struct page *);
	int i;
	void *addr;
	bool coherent = is_dma_coherent(dev, attrs);

	if (array_size <= PAGE_SIZE)
		pages = kzalloc(array_size, gfp);
	else
		pages = vzalloc(array_size);

	if (!pages)
		return NULL;

	if (coherent) {
		page = alloc_pages(gfp, get_order(size));
		addr = page ? page_address(page) : NULL;
	} else {
		addr = __alloc_from_pool(size, &page, gfp);
	}

	if (!addr)
		goto err_free;

	for (i = 0; i < count ; i++)
		pages[i] = page + i;

	*handle = __iommu_create_mapping(dev, pages, size, attrs);
	if (*handle == DMA_ERROR_CODE)
		goto err_mapping;

	kvfree(pages);
	return addr;

err_mapping:
	if (coherent)
		__free_pages(page, get_order(size));
	else
		__free_from_pool(addr, size);
err_free:
	kvfree(pages);
	return NULL;
}

static void __iommu_free_atomic(struct device *dev, void *cpu_addr,
				dma_addr_t handle, size_t size)
{
	__iommu_remove_mapping(dev, handle, size);
	__free_from_pool(cpu_addr, size);
}

static void *arm_iommu_alloc_attrs(struct device *dev, size_t size,
	    dma_addr_t *handle, gfp_t gfp, unsigned long attrs)
{
	bool coherent = is_dma_coherent(dev, attrs);
	pgprot_t prot = __get_dma_pgprot(attrs, PAGE_KERNEL, coherent);
	struct page **pages;
	void *addr = NULL;

	*handle = DMA_ERROR_CODE;
	size = PAGE_ALIGN(size);

	if (!gfpflags_allow_blocking(gfp))
		return __iommu_alloc_atomic(dev, size, handle, gfp, attrs);

	/*
	 * Following is a work-around (a.k.a. hack) to prevent pages
	 * with __GFP_COMP being passed to split_page() which cannot
	 * handle them.  The real problem is that this flag probably
	 * should be 0 on ARM as it is not supported on this
	 * platform; see CONFIG_HUGETLBFS.
	 */
	gfp &= ~(__GFP_COMP);

	pages = __iommu_alloc_buffer(dev, size, gfp, attrs);
	if (!pages)
		return NULL;

	*handle = __iommu_create_mapping(dev, pages, size, attrs);
	if (*handle == DMA_ERROR_CODE)
		goto err_buffer;

	if (attrs & DMA_ATTR_NO_KERNEL_MAPPING)
		return pages;

	addr = __iommu_alloc_remap(pages, size, gfp, prot,
				   __builtin_return_address(0));
	if (!addr)
		goto err_mapping;

	return addr;

err_mapping:
	__iommu_remove_mapping(dev, *handle, size);
err_buffer:
	__iommu_free_buffer(dev, pages, size, attrs);
	return NULL;
}

static int arm_iommu_mmap_attrs(struct device *dev, struct vm_area_struct *vma,
		    void *cpu_addr, dma_addr_t dma_addr, size_t size,
		    unsigned long attrs)
{
	unsigned long uaddr = vma->vm_start;
	unsigned long usize = vma->vm_end - vma->vm_start;
	struct page **pages = __iommu_get_pages(cpu_addr, attrs);
	bool coherent = is_dma_coherent(dev, attrs);

	vma->vm_page_prot = __get_dma_pgprot(attrs, vma->vm_page_prot,
					     coherent);

	if (!pages)
		return -ENXIO;

	do {
		int ret = vm_insert_page(vma, uaddr, *pages++);

		if (ret) {
			pr_err("Remapping memory failed: %d\n", ret);
			return ret;
		}
		uaddr += PAGE_SIZE;
		usize -= PAGE_SIZE;
	} while (usize > 0);

	return 0;
}

/*
 * free a page as defined by the above mapping.
 * Must not be called with IRQs disabled.
 */
void arm_iommu_free_attrs(struct device *dev, size_t size, void *cpu_addr,
			  dma_addr_t handle, unsigned long attrs)
{
	struct page **pages;

	size = PAGE_ALIGN(size);

	if (__in_atomic_pool(cpu_addr, size)) {
		__iommu_free_atomic(dev, cpu_addr, handle, size);
		return;
	}

	pages = __iommu_get_pages(cpu_addr, attrs);
	if (!pages) {
		WARN(1, "trying to free invalid coherent area: %p\n", cpu_addr);
		return;
	}

	if (!(attrs & DMA_ATTR_NO_KERNEL_MAPPING))
		dma_common_free_remap(cpu_addr, size, VM_USERMAP, true);

	__iommu_remove_mapping(dev, handle, size);
	__iommu_free_buffer(dev, pages, size, attrs);
}

int arm_iommu_get_sgtable(struct device *dev, struct sg_table *sgt,
				 void *cpu_addr, dma_addr_t dma_addr,
				 size_t size, unsigned long attrs)
{
	unsigned int count = PAGE_ALIGN(size) >> PAGE_SHIFT;
	struct page **pages = __iommu_get_pages(cpu_addr, attrs);

	if (!pages)
		return -ENXIO;

	return sg_alloc_table_from_pages(sgt, pages, count, 0, size,
					 GFP_KERNEL);
}

static int __dma_direction_to_prot(enum dma_data_direction dir)
{
	int prot;

	switch (dir) {
	case DMA_BIDIRECTIONAL:
		prot = IOMMU_READ | IOMMU_WRITE;
		break;
	case DMA_TO_DEVICE:
		prot = IOMMU_READ;
		break;
	case DMA_FROM_DEVICE:
		prot = IOMMU_WRITE;
		break;
	default:
		prot = 0;
	}

	return prot;
}

/**
 * arm_iommu_map_sg - map a set of SG buffers for streaming mode DMA
 * @dev: valid struct device pointer
 * @sg: list of buffers
 * @nents: number of buffers to map
 * @dir: DMA transfer direction
 *
 * Map a set of buffers described by scatterlist in streaming mode for DMA.
 * The scatter gather list elements are merged together (if possible) and
 * tagged with the appropriate dma address and length. They are obtained via
 * sg_dma_{address,length}.
 */
int arm_iommu_map_sg(struct device *dev, struct scatterlist *sg,
		int nents, enum dma_data_direction dir, unsigned long attrs)
{
	struct scatterlist *s;
	int ret, i;
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;
	unsigned int total_length = 0, current_offset = 0;
	dma_addr_t iova;
	int prot = __dma_direction_to_prot(dir);

	for_each_sg(sg, s, nents, i)
		total_length += s->length;

	iova = __alloc_iova(mapping, total_length);
	if (iova == DMA_ERROR_CODE) {
		dev_err(dev, "Couldn't allocate iova for sg %p\n", sg);
		return 0;
	}
	prot = __get_iommu_pgprot(attrs, prot,
				  is_dma_coherent(dev, attrs));

	ret = iommu_map_sg(mapping->domain, iova, sg, nents, prot);
	if (ret != total_length) {
		__free_iova(mapping, iova, total_length);
		return 0;
	}

	for_each_sg(sg, s, nents, i) {
		s->dma_address = iova + current_offset;
		s->dma_length = total_length - current_offset;
		current_offset += s->length;
	}

	return nents;
}

/**
 * arm_iommu_unmap_sg - unmap a set of SG buffers mapped by dma_map_sg
 * @dev: valid struct device pointer
 * @sg: list of buffers
 * @nents: number of buffers to unmap (same as was passed to dma_map_sg)
 * @dir: DMA transfer direction (same as was passed to dma_map_sg)
 *
 * Unmap a set of streaming mode DMA translations.  Again, CPU access
 * rules concerning calls here are the same as for dma_unmap_single().
 */
void arm_iommu_unmap_sg(struct device *dev, struct scatterlist *sg, int nents,
			enum dma_data_direction dir, unsigned long attrs)
{
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;
	unsigned int total_length = sg_dma_len(sg);
	dma_addr_t iova = sg_dma_address(sg);

	total_length = PAGE_ALIGN((iova & ~PAGE_MASK) + total_length);
	iova &= PAGE_MASK;

	iommu_unmap(mapping->domain, iova, total_length);
	__free_iova(mapping, iova, total_length);
}

/**
 * arm_iommu_sync_sg_for_cpu
 * @dev: valid struct device pointer
 * @sg: list of buffers
 * @nents: number of buffers to map (returned from dma_map_sg)
 * @dir: DMA transfer direction (same as was passed to dma_map_sg)
 */
void arm_iommu_sync_sg_for_cpu(struct device *dev, struct scatterlist *sg,
			int nents, enum dma_data_direction dir)
{
	struct scatterlist *s;
	int i;
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;
	dma_addr_t iova = sg_dma_address(sg);
	bool iova_coherent = iommu_is_iova_coherent(mapping->domain, iova);

	if (iova_coherent)
		return;

	for_each_sg(sg, s, nents, i)
		__dma_page_dev_to_cpu(sg_page(s), s->offset, s->length, dir);

}

/**
 * arm_iommu_sync_sg_for_device
 * @dev: valid struct device pointer
 * @sg: list of buffers
 * @nents: number of buffers to map (returned from dma_map_sg)
 * @dir: DMA transfer direction (same as was passed to dma_map_sg)
 */
void arm_iommu_sync_sg_for_device(struct device *dev, struct scatterlist *sg,
			int nents, enum dma_data_direction dir)
{
	struct scatterlist *s;
	int i;
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;
	dma_addr_t iova = sg_dma_address(sg);
	bool iova_coherent = iommu_is_iova_coherent(mapping->domain, iova);

	if (iova_coherent)
		return;

	for_each_sg(sg, s, nents, i)
		__dma_page_cpu_to_dev(sg_page(s), s->offset, s->length, dir);
}


/**
 * arm_coherent_iommu_map_page
 * @dev: valid struct device pointer
 * @page: page that buffer resides in
 * @offset: offset into page for start of buffer
 * @size: size of buffer to map
 * @dir: DMA transfer direction
 *
 * Coherent IOMMU aware version of arm_dma_map_page()
 */
static dma_addr_t arm_coherent_iommu_map_page(struct device *dev,
	     struct page *page, unsigned long offset, size_t size,
	     enum dma_data_direction dir, unsigned long attrs)
{
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;
	dma_addr_t dma_addr;
	int ret, prot, len, start_offset, map_offset;

	map_offset = offset & ~PAGE_MASK;
	start_offset = offset & PAGE_MASK;
	len = PAGE_ALIGN(map_offset + size);

	dma_addr = __alloc_iova(mapping, len);
	if (dma_addr == DMA_ERROR_CODE)
		return dma_addr;

	prot = __dma_direction_to_prot(dir);
	prot = __get_iommu_pgprot(attrs, prot,
				  is_dma_coherent(dev, attrs));

	ret = iommu_map(mapping->domain, dma_addr, page_to_phys(page) +
			start_offset, len, prot);
	if (ret < 0)
		goto fail;

	return dma_addr + map_offset;
fail:
	__free_iova(mapping, dma_addr, len);
	return DMA_ERROR_CODE;
}

/**
 * arm_iommu_map_page
 * @dev: valid struct device pointer
 * @page: page that buffer resides in
 * @offset: offset into page for start of buffer
 * @size: size of buffer to map
 * @dir: DMA transfer direction
 *
 * IOMMU aware version of arm_dma_map_page()
 */
static dma_addr_t arm_iommu_map_page(struct device *dev, struct page *page,
	     unsigned long offset, size_t size, enum dma_data_direction dir,
	     unsigned long attrs)
{
	if (!is_dma_coherent(dev, attrs) &&
	      !(attrs & DMA_ATTR_SKIP_CPU_SYNC))
		__dma_page_cpu_to_dev(page, offset, size, dir);

	return arm_coherent_iommu_map_page(dev, page, offset, size, dir, attrs);
}

/**
 * arm_iommu_unmap_page
 * @dev: valid struct device pointer
 * @handle: DMA address of buffer
 * @size: size of buffer (same as passed to dma_map_page)
 * @dir: DMA transfer direction (same as passed to dma_map_page)
 *
 * IOMMU aware version of arm_dma_unmap_page()
 */
static void arm_iommu_unmap_page(struct device *dev, dma_addr_t handle,
		size_t size, enum dma_data_direction dir,
		unsigned long attrs)
{
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;
	dma_addr_t iova = handle & PAGE_MASK;
	struct page *page = phys_to_page(iommu_iova_to_phys(
						mapping->domain, iova));
	int offset = handle & ~PAGE_MASK;
	int len = PAGE_ALIGN(size + offset);

	if (!(is_dma_coherent(dev, attrs) ||
	      (attrs & DMA_ATTR_SKIP_CPU_SYNC)))
		__dma_page_dev_to_cpu(page, offset, size, dir);

	iommu_unmap(mapping->domain, iova, len);
	__free_iova(mapping, iova, len);
}

static void arm_iommu_sync_single_for_cpu(struct device *dev,
		dma_addr_t handle, size_t size, enum dma_data_direction dir)
{
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;
	dma_addr_t iova = handle & PAGE_MASK;
	struct page *page = phys_to_page(iommu_iova_to_phys(
						mapping->domain, iova));
	unsigned int offset = handle & ~PAGE_MASK;
	bool iova_coherent = iommu_is_iova_coherent(mapping->domain, handle);

	if (!iova_coherent)
		__dma_page_dev_to_cpu(page, offset, size, dir);
}

static void arm_iommu_sync_single_for_device(struct device *dev,
		dma_addr_t handle, size_t size, enum dma_data_direction dir)
{
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;
	dma_addr_t iova = handle & PAGE_MASK;
	struct page *page = phys_to_page(iommu_iova_to_phys(
						mapping->domain, iova));
	unsigned int offset = handle & ~PAGE_MASK;
	bool iova_coherent = iommu_is_iova_coherent(mapping->domain, handle);

	if (!iova_coherent)
		__dma_page_cpu_to_dev(page, offset, size, dir);
}

static dma_addr_t arm_iommu_dma_map_resource(
			struct device *dev, phys_addr_t phys_addr,
			size_t size, enum dma_data_direction dir,
			unsigned long attrs)
{
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;
	size_t offset = phys_addr & ~PAGE_MASK;
	size_t len = PAGE_ALIGN(size + offset);
	dma_addr_t dma_addr;
	int prot;

	dma_addr = __alloc_iova(mapping, len);
	if (dma_addr == DMA_ERROR_CODE)
		return dma_addr;

	prot = __dma_direction_to_prot(dir);
	prot |= IOMMU_MMIO;

	if (iommu_map(mapping->domain, dma_addr, phys_addr - offset,
			len, prot)) {
		__free_iova(mapping, dma_addr, len);
		return DMA_ERROR_CODE;
	}
	return dma_addr + offset;
}

static void arm_iommu_dma_unmap_resource(
			struct device *dev, dma_addr_t addr,
			size_t size, enum dma_data_direction dir,
			unsigned long attrs)
{
	struct dma_iommu_mapping *mapping = dev->archdata.mapping;
	size_t offset = addr & ~PAGE_MASK;
	size_t len = PAGE_ALIGN(size + offset);

	iommu_unmap(mapping->domain, addr - offset, len);
	__free_iova(mapping, addr - offset, len);
}

static int arm_iommu_mapping_error(struct device *dev,
				   dma_addr_t dma_addr)
{
	return dma_addr == DMA_ERROR_CODE;
}

const struct dma_map_ops iommu_ops = {
	.alloc		= arm_iommu_alloc_attrs,
	.free		= arm_iommu_free_attrs,
	.mmap		= arm_iommu_mmap_attrs,
	.get_sgtable	= arm_iommu_get_sgtable,

	.map_page		= arm_iommu_map_page,
	.unmap_page		= arm_iommu_unmap_page,
	.sync_single_for_cpu	= arm_iommu_sync_single_for_cpu,
	.sync_single_for_device	= arm_iommu_sync_single_for_device,

	.map_sg			= arm_iommu_map_sg,
	.unmap_sg		= arm_iommu_unmap_sg,
	.sync_sg_for_cpu	= arm_iommu_sync_sg_for_cpu,
	.sync_sg_for_device	= arm_iommu_sync_sg_for_device,

	.map_resource		= arm_iommu_dma_map_resource,
	.unmap_resource		= arm_iommu_dma_unmap_resource,

	.mapping_error		= arm_iommu_mapping_error,
};

/**
 * arm_iommu_create_mapping
 * @bus: pointer to the bus holding the client device (for IOMMU calls)
 * @base: start address of the valid IO address space
 * @size: maximum size of the valid IO address space
 *
 * Creates a mapping structure which holds information about used/unused
 * IO address ranges, which is required to perform memory allocation and
 * mapping with IOMMU aware functions.
 *
 * Clients may use iommu_domain_set_attr() to set additional flags prior
 * to calling arm_iommu_attach_device() to complete initialization.
 */
struct dma_iommu_mapping *
arm_iommu_create_mapping(struct bus_type *bus, dma_addr_t base, size_t size)
{
	unsigned int bits = size >> PAGE_SHIFT;
	struct dma_iommu_mapping *mapping;

	if (!bits)
		return ERR_PTR(-EINVAL);

	mapping = kzalloc(sizeof(struct dma_iommu_mapping), GFP_KERNEL);
	if (!mapping)
		return ERR_PTR(-ENOMEM);

	mapping->base = base;
	mapping->bits = bits;

	mapping->domain = iommu_domain_alloc(bus);
	if (!mapping->domain)
		goto err_domain_alloc;

	mapping->init = false;
	return mapping;

err_domain_alloc:
	kfree(mapping);
	return ERR_PTR(-ENOMEM);
}
EXPORT_SYMBOL(arm_iommu_create_mapping);

static int
iommu_init_mapping(struct device *dev, struct dma_iommu_mapping *mapping)
{
	struct iommu_domain *domain = mapping->domain;
	dma_addr_t dma_base = mapping->base;
	u64 size = mapping->bits << PAGE_SHIFT;

	/* Prepare the domain */
	if (iommu_get_dma_cookie(domain))
		return -EINVAL;

	if (iommu_dma_init_domain(domain, dma_base, size, dev))
		goto out_put_cookie;

	mapping->ops = &iommu_dma_ops;
	return 0;

out_put_cookie:
	iommu_put_dma_cookie(domain);
	return -EINVAL;
}

static int
bitmap_iommu_init_mapping(struct device *dev, struct dma_iommu_mapping *mapping)
{
	unsigned int bitmap_size = BITS_TO_LONGS(mapping->bits) * sizeof(long);
	int vmid = VMID_HLOS;
	int min_iova_align = 0;

	iommu_domain_get_attr(mapping->domain,
			DOMAIN_ATTR_QCOM_MMU500_ERRATA_MIN_IOVA_ALIGN,
			&min_iova_align);
	iommu_domain_get_attr(mapping->domain,
			DOMAIN_ATTR_SECURE_VMID, &vmid);
	if (vmid >= VMID_LAST || vmid < 0)
		vmid = VMID_HLOS;

	if (min_iova_align) {
		mapping->min_iova_align = ARM_SMMU_MIN_IOVA_ALIGN;
		mapping->guard_page = arm_smmu_errata_get_guard_page(vmid);
		if (!mapping->guard_page)
			return -ENOMEM;
	}

	mapping->bitmap = kzalloc(bitmap_size, GFP_KERNEL | __GFP_NOWARN |
				__GFP_NORETRY);
	if (!mapping->bitmap)
		mapping->bitmap = vzalloc(bitmap_size);

	if (!mapping->bitmap)
		return -ENOMEM;

	spin_lock_init(&mapping->lock);
	mapping->ops = &iommu_ops;
	return 0;
}

static void release_iommu_mapping(struct kref *kref)
{
	int is_bitmap = 0;
	struct dma_iommu_mapping *mapping =
		container_of(kref, struct dma_iommu_mapping, kref);

	iommu_domain_get_attr(mapping->domain,
				DOMAIN_ATTR_BITMAP_IOVA_ALLOCATOR, &is_bitmap);
	if (is_bitmap)
		kfree(mapping->bitmap);
	iommu_domain_free(mapping->domain);
	kfree(mapping);
}

/*
 * arm_iommu_release_mapping
 * @mapping: allocted via arm_iommu_create_mapping()
 *
 * Frees all resources associated with the iommu mapping.
 * The device associated with this mapping must be in the 'detached' state
 */
void arm_iommu_release_mapping(struct dma_iommu_mapping *mapping)
{
	int is_fast = 0;
	void (*release)(struct kref *kref);

	if (!mapping)
		return;

	if (!mapping->init) {
		iommu_domain_free(mapping->domain);
		kfree(mapping);
		return;
	}

	iommu_domain_get_attr(mapping->domain, DOMAIN_ATTR_FAST, &is_fast);

	if (is_fast)
		release = fast_smmu_release_mapping;
	else
		release = release_iommu_mapping;

	kref_put(&mapping->kref, release);
}
EXPORT_SYMBOL(arm_iommu_release_mapping);

static int arm_iommu_init_mapping(struct device *dev,
			    struct dma_iommu_mapping *mapping)
{
	int err = -EINVAL;
	int s1_bypass = 0, is_fast = 0, is_bitmap = 0;
	dma_addr_t iova_end;

	if (mapping->init)
		return 0;

	iova_end = mapping->base + (mapping->bits << PAGE_SHIFT) - 1;
	if (iova_end > dma_get_mask(dev)) {
		dev_err(dev, "dma mask %llx too small for requested iova range %pad to %pad\n",
			dma_get_mask(dev), &mapping->base, &iova_end);
		return -EINVAL;
	}

	iommu_domain_get_attr(mapping->domain, DOMAIN_ATTR_S1_BYPASS,
					&s1_bypass);
	iommu_domain_get_attr(mapping->domain, DOMAIN_ATTR_FAST, &is_fast);
	iommu_domain_get_attr(mapping->domain,
			DOMAIN_ATTR_BITMAP_IOVA_ALLOCATOR, &is_bitmap);

	if (s1_bypass) {
		mapping->ops = &swiotlb_dma_ops;
		err = 0;
	} else if (is_fast)
		err = fast_smmu_init_mapping(dev, mapping);
	else if (is_bitmap)
		err = bitmap_iommu_init_mapping(dev, mapping);
	else
		err = iommu_init_mapping(dev, mapping);
	if (!err) {
		kref_init(&mapping->kref);
		mapping->init = true;
	}
	return err;
}

/**
 * arm_iommu_attach_device
 * @dev: valid struct device pointer
 * @mapping: io address space mapping structure (returned from
 *	arm_iommu_create_mapping)
 *
 * Attaches specified io address space mapping to the provided device,
 * this replaces the dma operations (dma_map_ops pointer) with the
 * IOMMU aware version.
 *
 * Clients are expected to call arm_iommu_attach_device() prior to sharing
 * the dma_iommu_mapping structure with another device. This ensures
 * initialization is complete.
 */
int arm_iommu_attach_device(struct device *dev,
			    struct dma_iommu_mapping *mapping)
{
	int err;
	struct iommu_domain *domain = mapping->domain;
	struct iommu_group *group = dev->iommu_group;

	if (!group) {
		dev_err(dev, "No iommu associated with device\n");
		return -EINVAL;
	}

	if (iommu_get_domain_for_dev(dev)) {
		dev_err(dev, "Device already attached to other iommu_domain\n");
		return -EINVAL;
	}

	err = iommu_attach_group(mapping->domain, group);
	if (err)
		return err;

	err = arm_iommu_init_mapping(dev, mapping);
	if (err) {
		iommu_detach_group(domain, group);
		return err;
	}

	dev->archdata.mapping = mapping;
	set_dma_ops(dev, mapping->ops);

	pr_debug("Attached IOMMU controller to %s device.\n", dev_name(dev));
	return 0;
}
EXPORT_SYMBOL(arm_iommu_attach_device);

/**
 * arm_iommu_detach_device
 * @dev: valid struct device pointer
 *
 * Detaches the provided device from a previously attached map.
 * This voids the dma operations (dma_map_ops pointer)
 */
void arm_iommu_detach_device(struct device *dev)
{
	struct dma_iommu_mapping *mapping;
	int s1_bypass = 0;

	mapping = to_dma_iommu_mapping(dev);
	if (!mapping) {
		dev_warn(dev, "Not attached\n");
		return;
	}

	if (!dev->iommu_group) {
		dev_err(dev, "No iommu associated with device\n");
		return;
	}

	iommu_domain_get_attr(mapping->domain, DOMAIN_ATTR_S1_BYPASS,
					&s1_bypass);

	/*
	 * ION defers dma_unmap calls. Ensure they have all completed prior to
	 * setting dma_ops to NULL.
	 */
	msm_dma_unmap_all_for_dev(dev);

	iommu_detach_group(mapping->domain, dev->iommu_group);
	dev->archdata.mapping = NULL;
	if (!s1_bypass)
		set_dma_ops(dev, NULL);

	pr_debug("Detached IOMMU controller from %s device.\n", dev_name(dev));
}
EXPORT_SYMBOL(arm_iommu_detach_device);

#endif
