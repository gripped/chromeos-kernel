/*
 * Copyright 2011 Red Hat, Inc.
 * Copyright © 2014 The Chromium OS Authors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software")
 * to deal in the software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * them Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTIBILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES, OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT, OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *	Adam Jackson <ajax@redhat.com>
 *	Ben Widawsky <ben@bwidawsk.net>
 */

/**
 * This is vgem, a (non-hardware-backed) GEM service.  This is used by Mesa's
 * software renderer and the X server for efficient buffer sharing.
 */

#include "drmP.h"
#include <linux/dma-buf.h>
#include <linux/module.h>
#include <linux/ramfs.h>
#include <linux/shmem_fs.h>
#include "vgem_drv.h"

#define DRIVER_NAME	"vgem"
#define DRIVER_DESC	"Virtual GEM provider"
#define DRIVER_DATE	"20120112"
#define DRIVER_MAJOR	1
#define DRIVER_MINOR	0

static int vgem_load(struct drm_device *dev, unsigned long flags)
{
	return 0;
}

static int vgem_unload(struct drm_device *dev)
{
	return 0;
}

static int vgem_open(struct drm_device *dev, struct drm_file *file)
{
	return 0;
}

static void vgem_preclose(struct drm_device *dev, struct drm_file *file)
{
}

static void vgem_lastclose(struct drm_device *dev)
{
}

static int vgem_gem_init_object(struct drm_gem_object *obj)
{
	return 0;
}

void vgem_gem_put_pages(struct drm_vgem_gem_object *obj)
{
	int num_pages = obj->base.size / PAGE_SIZE;
	int i;

	for (i = 0; i < num_pages; i++) {
		if (obj->pages[i] == NULL)
			break;
		page_cache_release(obj->pages[i]);
	}

	drm_free_large(obj->pages);
	obj->pages = NULL;
}

static void vgem_gem_free_object(struct drm_gem_object *obj)
{
	struct drm_vgem_gem_object *vgem_obj = to_vgem_bo(obj);

	if (obj->map_list.map)
		drm_gem_free_mmap_offset(obj);

	if (obj->import_attach) {
		dma_buf_put(obj->import_attach->dmabuf);
		obj->dma_buf = NULL;
	}

	drm_gem_object_release(obj);

	if (vgem_obj->pages)
		vgem_gem_put_pages(vgem_obj);

	vgem_obj->pages = NULL;

	if (vgem_obj->sgt)
		sg_free_table(vgem_obj->sgt);

	vgem_obj->sgt = NULL;

	kfree(vgem_obj);
}

int vgem_gem_get_pages(struct drm_vgem_gem_object *obj)
{
	struct scatterlist *s;
	struct address_space *mapping;
	gfp_t gfpmask = GFP_KERNEL;
	int num_pages, i, ret = 0;

	if (obj->pages || obj->base.import_attach)
		return 0;

	num_pages = obj->base.size / PAGE_SIZE;
	obj->pages = drm_malloc_ab(num_pages, sizeof(struct page *));
	if (obj->pages == NULL)
		return -ENOMEM;

	mapping = obj->base.filp->f_path.dentry->d_inode->i_mapping;
	gfpmask |= mapping_gfp_mask(mapping);

	for (i = 0; i < num_pages; i++) {
		struct page *page;
		obj->pages[i] = NULL;
		page = shmem_read_mapping_page_gfp(mapping, i, gfpmask);
		if (IS_ERR(page)) {
			ret = PTR_ERR(page);
			goto err_out;
		}
		obj->pages[i] = page;
	}

	obj->sgt = drm_prime_pages_to_sg(obj->pages, num_pages);
	if (IS_ERR(obj->sgt))
		goto err_out;

	for_each_sg(obj->sgt->sgl, s, obj->sgt->nents, i)
		sg_dma_address(s) = sg_phys(s);

	drm_clflush_sg(obj->base.dev, obj->sgt);

	return ret;

err_out:
	vgem_gem_put_pages(obj);
	return ret;
}

static int vgem_gem_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct drm_vgem_gem_object *obj = to_vgem_bo(vma->vm_private_data);
	struct drm_device *dev = obj->base.dev;
	loff_t num_pages;
	pgoff_t page_offset;
	int ret;

	/* We don't use vmf->pgoff since that has the fake offset */
	page_offset = ((unsigned long)vmf->virtual_address - vma->vm_start) >>
		PAGE_SHIFT;

	num_pages = DIV_ROUND_UP(obj->base.size, PAGE_SIZE);

	if (page_offset > num_pages)
		return VM_FAULT_SIGBUS;

	mutex_lock(&dev->struct_mutex);

	ret = vm_insert_page(vma, (unsigned long)vmf->virtual_address,
			     obj->pages[page_offset]);

	mutex_unlock(&dev->struct_mutex);
	switch (ret) {
	case 0:
		return VM_FAULT_NOPAGE;
	case -ENOMEM:
		return VM_FAULT_OOM;
	case -EBUSY:
		return VM_FAULT_RETRY;
	case -EFAULT:
	case -EINVAL:
		return VM_FAULT_SIGBUS;
	default:
		WARN_ON(1);
		return VM_FAULT_SIGBUS;
	}
}

static struct vm_operations_struct vgem_gem_vm_ops = {
	.fault = vgem_gem_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

/* ioctls */

static struct drm_gem_object *vgem_gem_create(struct drm_device *dev,
					      struct drm_file *file,
					      unsigned int *handle,
					      unsigned long size)
{
	struct drm_vgem_gem_object *obj;
	struct drm_gem_object *gem_object;
	int err;

	size = roundup(size, PAGE_SIZE);

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return ERR_PTR(-ENOMEM);

	gem_object = &obj->base;

	err = drm_gem_object_init(dev, gem_object, size);
	if (err)
		goto out;

	err = drm_gem_handle_create(file, gem_object, handle);
	if (err)
		goto handle_out;

	drm_gem_object_unreference_unlocked(gem_object);

	return gem_object;

handle_out:
	drm_gem_object_release(gem_object);
out:
	kfree(obj);
	return ERR_PTR(err);
}

static int vgem_gem_dumb_create(struct drm_file *file, struct drm_device *dev,
				struct drm_mode_create_dumb *args)
{
	struct drm_gem_object *gem_object;
	uint64_t size;
	uint64_t pitch = args->width * DIV_ROUND_UP(args->bpp, 8);

	size = args->height * pitch;
	if (size == 0)
		return -EINVAL;

	gem_object = vgem_gem_create(dev, file, &args->handle, size);

	if (IS_ERR(gem_object)) {
		DRM_DEBUG_DRIVER("object creation failed\n");
		return PTR_ERR(gem_object);
	}

	args->size = gem_object->size;
	args->pitch = pitch;

	DRM_DEBUG_DRIVER("Created object of size %lld\n", size);

	return 0;
}

int vgem_gem_dumb_map(struct drm_file *file, struct drm_device *dev,
		      uint32_t handle, uint64_t *offset)
{
	int ret = 0;
	struct drm_gem_object *obj;

	mutex_lock(&dev->struct_mutex);
	obj = drm_gem_object_lookup(dev, file, handle);
	if (!obj) {
		ret = -ENOENT;
		goto unlock;
	}

	if (!obj->map_list.map) {
		ret = drm_gem_create_mmap_offset(obj);
		if (ret)
			goto unref;
	}

	BUG_ON(!obj->filp);

	obj->filp->private_data = obj;

	BUG_ON(!obj->map_list.map);

	ret = vgem_gem_get_pages(to_vgem_bo(obj));
	if (ret)
		goto fail_get_pages;

	*offset = (uint64_t)obj->map_list.hash.key << PAGE_SHIFT;

	goto unref;

fail_get_pages:
	drm_gem_free_mmap_offset(obj);
unref:
	drm_gem_object_unreference(obj);
unlock:
	mutex_unlock(&dev->struct_mutex);
	return ret;
}


int vgem_drm_gem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct drm_file *priv = filp->private_data;
	struct drm_device *dev = priv->minor->dev;
	struct drm_gem_mm *mm = dev->mm_private;
	struct drm_local_map *map = NULL;
	struct drm_gem_object *obj;
	struct drm_vgem_gem_object *vgem_obj;
	struct drm_hash_item *hash;
	int ret = 0;

	mutex_lock(&dev->struct_mutex);

	if (drm_ht_find_item(&mm->offset_hash, vma->vm_pgoff, &hash)) {
		mutex_unlock(&dev->struct_mutex);
		return drm_mmap(filp, vma);
	}

	map = drm_hash_entry(hash, struct drm_map_list, hash)->map;
	if (!map ||
	    ((map->flags & _DRM_RESTRICTED) && !capable(CAP_SYS_ADMIN))) {
		ret =  -EPERM;
		goto out_unlock;
	}

	/* Check for valid size. */
	if (map->size < vma->vm_end - vma->vm_start) {
		ret = -EINVAL;
		goto out_unlock;
	}

	obj = map->handle;
	vgem_obj = to_vgem_bo(obj);

	if (obj->import_attach) {
		ret = dma_buf_mmap(obj->import_attach->dmabuf, vma, 0);
		goto out_unlock;
	}

	if (!obj->dev->driver->gem_vm_ops) {
		ret = -EINVAL;
		goto out_unlock;
	}

	vma->vm_flags |= VM_IO | VM_MIXEDMAP | VM_DONTEXPAND | VM_DONTDUMP;
	vma->vm_ops = obj->dev->driver->gem_vm_ops;
	vma->vm_private_data = map->handle;
	vma->vm_page_prot =
		pgprot_writecombine(vm_get_page_prot(vma->vm_flags));

	/* Take a ref for this mapping of the object, so that the fault
	 * handler can dereference the mmap offset's pointer to the object.
	 * This reference is cleaned up by the corresponding vm_close
	 * (which should happen whether the vma was created by this call, or
	 * by a vm_open due to mremap or partial unmap or whatever).
	 */

	mutex_unlock(&dev->struct_mutex);
	drm_gem_vm_open(vma);
	return ret;

out_unlock:
	mutex_unlock(&dev->struct_mutex);

	return ret;
}


static struct drm_ioctl_desc vgem_ioctls[] = {
	DRM_IOCTL_DEF_DRV(VGEM_MODE_MAP_DUMB, drm_mode_mmap_dumb_ioctl,
			  DRM_CONTROL_ALLOW|DRM_UNLOCKED|DRM_RENDER_ALLOW),
};

static const struct file_operations vgem_driver_fops = {
	.owner		= THIS_MODULE,
	.open		= drm_open,
	.mmap		= vgem_drm_gem_mmap,
	.poll		= drm_poll,
	.read		= drm_read,
	.unlocked_ioctl = drm_ioctl,
	.release	= drm_release,
};

static struct drm_driver vgem_driver = {
	.driver_features	=
		DRIVER_BUS_PLATFORM | DRIVER_GEM | DRIVER_PRIME | DRIVER_RENDER,
	.load			= vgem_load,
	.unload			= vgem_unload,
	.open			= vgem_open,
	.preclose		= vgem_preclose,
	.lastclose		= vgem_lastclose,
	.gem_init_object	= vgem_gem_init_object,
	.gem_free_object	= vgem_gem_free_object,
	.gem_vm_ops		= &vgem_gem_vm_ops,
	.ioctls			= vgem_ioctls,
	.fops			= &vgem_driver_fops,
	.dumb_create		= vgem_gem_dumb_create,
	.dumb_map_offset	= vgem_gem_dumb_map,
	.prime_handle_to_fd	= drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle	= drm_gem_prime_fd_to_handle,
	.gem_prime_export	= drm_gem_prime_export,
	.gem_prime_import	= drm_gem_prime_import,
	.gem_prime_pin			= vgem_gem_prime_pin,
	.gem_prime_get_sg_table		= vgem_gem_prime_get_sg_table,
	.gem_prime_import_sg_table	= vgem_gem_prime_import_sg_table,
	.gem_prime_vmap			= vgem_gem_prime_vmap,
	.gem_prime_vunmap		= vgem_gem_prime_vunmap,
	.gem_prime_mmap			= vgem_gem_prime_mmap,
	.name	= DRIVER_NAME,
	.desc	= DRIVER_DESC,
	.date	= DRIVER_DATE,
	.major	= DRIVER_MAJOR,
	.minor	= DRIVER_MINOR,
};

static int vgem_platform_probe(struct platform_device *pdev)
{
	vgem_driver.num_ioctls = ARRAY_SIZE(vgem_ioctls);

	return drm_platform_init(&vgem_driver, pdev);
}

static int vgem_platform_remove(struct platform_device *pdev)
{
	drm_platform_exit(&vgem_driver, pdev);

	return 0;
}

static struct platform_driver vgem_platform_driver = {
	.probe		= vgem_platform_probe,
	.remove		= vgem_platform_remove,
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= DRIVER_NAME,
	},
};

static struct platform_device *vgem_device;

static int __init vgem_init(void)
{
	int ret;

	ret = platform_driver_register(&vgem_platform_driver);
	if (ret)
		goto out;

	vgem_device = platform_device_alloc("vgem", -1);
	if (!vgem_device) {
		ret = -ENOMEM;
		goto unregister_out;
	}

	vgem_device->dev.coherent_dma_mask = ~0ULL;
	vgem_device->dev.dma_mask = &vgem_device->dev.coherent_dma_mask;

	ret = platform_device_add(vgem_device);
	if (ret)
		goto put_out;

	return 0;

put_out:
	platform_device_put(vgem_device);
unregister_out:
	platform_driver_unregister(&vgem_platform_driver);
out:
	return ret;
}

static void __exit vgem_exit(void)
{
	platform_device_unregister(vgem_device);
	platform_driver_unregister(&vgem_platform_driver);
}

module_init(vgem_init);
module_exit(vgem_exit);

MODULE_AUTHOR("Red Hat, Inc.");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL and additional rights");
