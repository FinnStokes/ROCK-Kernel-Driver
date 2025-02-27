/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <linux/dma-buf.h>
#include <linux/slab.h>
#include <linux/random.h>

#include "kfd_ipc.h"
#include "kfd_priv.h"
#include "amdgpu_amdkfd.h"

#define KFD_IPC_HASH_TABLE_SIZE_SHIFT 4
#define KFD_IPC_HASH_TABLE_SIZE_MASK ((1 << KFD_IPC_HASH_TABLE_SIZE_SHIFT) - 1)

static struct kfd_ipc_handles {
	DECLARE_HASHTABLE(handles, KFD_IPC_HASH_TABLE_SIZE_SHIFT);
	struct mutex lock;
} kfd_ipc_handles;

/* Since, handles are random numbers, it can be used directly as hashing key.
 * The least 4 bits of the handle are used as key. However, during import all
 * 128 bits of the handle are checked to prevent handle snooping.
 */
#define HANDLE_TO_KEY(sh) ((*(uint64_t *)sh) & KFD_IPC_HASH_TABLE_SIZE_MASK)

static int ipc_store_insert(void *val, void *sh, struct kfd_ipc_obj **ipc_obj)
{
	struct kfd_ipc_obj *obj;

	obj = kmalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return -ENOMEM;

	/* The initial ref belongs to the allocator process.
	 * The IPC object store itself does not hold a ref since
	 * there is no specific moment in time where that ref should
	 * be dropped, except "when there are no more userspace processes
	 * holding a ref to the object". Therefore the removal from IPC
	 * storage happens at ipc_obj release time.
	 */
	kref_init(&obj->ref);
	obj->data = val;
	get_random_bytes(obj->share_handle, sizeof(obj->share_handle));

	memcpy(sh, obj->share_handle, sizeof(obj->share_handle));

	mutex_lock(&kfd_ipc_handles.lock);
	hlist_add_head(&obj->node,
		&kfd_ipc_handles.handles[HANDLE_TO_KEY(obj->share_handle)]);
	mutex_unlock(&kfd_ipc_handles.lock);

	if (ipc_obj)
		*ipc_obj = obj;

	return 0;
}

static void ipc_obj_release(struct kref *r)
{
	struct kfd_ipc_obj *obj;

	obj = container_of(r, struct kfd_ipc_obj, ref);

	mutex_lock(&kfd_ipc_handles.lock);
	hash_del(&obj->node);
	mutex_unlock(&kfd_ipc_handles.lock);

	dma_buf_put(obj->data);
	kfree(obj);
}

void ipc_obj_get(struct kfd_ipc_obj *obj)
{
	kref_get(&obj->ref);
}

void ipc_obj_put(struct kfd_ipc_obj **obj)
{
	kref_put(&(*obj)->ref, ipc_obj_release);
	*obj = NULL;
}

int kfd_ipc_init(void)
{
	mutex_init(&kfd_ipc_handles.lock);
	hash_init(kfd_ipc_handles.handles);
	return 0;
}

static int kfd_import_dmabuf_create_kfd_bo(struct kfd_dev *dev,
			  struct kfd_process *p,
			  uint32_t gpu_id, struct dma_buf *dmabuf,
			  uint64_t va_addr, uint64_t *handle,
			  uint64_t *mmap_offset,
			  struct kfd_ipc_obj *ipc_obj)
{
	int r;
	void *mem;
	uint64_t size;
	int idr_handle;
	struct kfd_process_device *pdd = NULL;

	if (!handle)
		return -EINVAL;

	if (!dev)
		return -EINVAL;

	mutex_lock(&p->mutex);

	pdd = kfd_bind_process_to_device(dev, p);
	if (IS_ERR(pdd)) {
		r = PTR_ERR(pdd);
		goto err_unlock;
	}

	r = amdgpu_amdkfd_gpuvm_import_dmabuf(dev->kgd, dmabuf,
					va_addr, pdd->vm,
					(struct kgd_mem **)&mem, &size,
					mmap_offset);
	if (r)
		goto err_unlock;

	idr_handle = kfd_process_device_create_obj_handle(pdd, mem,
							  va_addr, size, 0, 0,
							  ipc_obj);
	if (idr_handle < 0) {
		r = -EFAULT;
		goto err_free;
	}

	mutex_unlock(&p->mutex);

	*handle = MAKE_HANDLE(gpu_id, idr_handle);

	return 0;

err_free:
	amdgpu_amdkfd_gpuvm_free_memory_of_gpu(dev->kgd,
					 (struct kgd_mem *)mem);
err_unlock:
	mutex_unlock(&p->mutex);
	return r;
}

int kfd_ipc_import_dmabuf(struct kfd_dev *dev,
					   struct kfd_process *p,
					   uint32_t gpu_id, int dmabuf_fd,
					   uint64_t va_addr, uint64_t *handle,
					   uint64_t *mmap_offset)
{
	int r;
	struct dma_buf *dmabuf = dma_buf_get(dmabuf_fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	r = kfd_import_dmabuf_create_kfd_bo(dev, p, gpu_id, dmabuf,
					    va_addr, handle, mmap_offset,
					    NULL);
	dma_buf_put(dmabuf);
	return r;
}

int kfd_ipc_import_handle(struct kfd_dev *dev, struct kfd_process *p,
			  uint32_t gpu_id, uint32_t *share_handle,
			  uint64_t va_addr, uint64_t *handle,
			  uint64_t *mmap_offset)
{
	int r;
	struct kfd_ipc_obj *entry, *found = NULL;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 9, 0)
	struct hlist_node *tmp_node;
#endif

	mutex_lock(&kfd_ipc_handles.lock);
	/* Convert the user provided handle to hash key and search only in that
	 * bucket
	 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 9, 0)
	hlist_for_each_entry(entry, tmp_node,
		&kfd_ipc_handles.handles[HANDLE_TO_KEY(share_handle)], node) {
#else
	hlist_for_each_entry(entry,
		&kfd_ipc_handles.handles[HANDLE_TO_KEY(share_handle)], node) {
#endif
		if (!memcmp(entry->share_handle, share_handle,
			    sizeof(entry->share_handle))) {
			found = entry;
			break;
		}
	}
	mutex_unlock(&kfd_ipc_handles.lock);

	if (!found)
		return -EINVAL;
	ipc_obj_get(found);

	pr_debug("Found ipc_dma_buf: %p\n", found->data);

	r = kfd_import_dmabuf_create_kfd_bo(dev, p, gpu_id, found->data,
					    va_addr, handle, mmap_offset,
					    found);
	if (r)
		goto error_unref;

	return r;

error_unref:
	ipc_obj_put(&found);
	return r;
}

int kfd_ipc_export_as_handle(struct kfd_dev *dev, struct kfd_process *p,
			     uint64_t handle, uint32_t *ipc_handle)
{
	struct kfd_process_device *pdd = NULL;
	struct kfd_ipc_obj *obj;
	struct kfd_bo *kfd_bo = NULL;
	struct dma_buf *dmabuf;
	int r;

	if (!dev || !ipc_handle)
		return -EINVAL;

	mutex_lock(&p->mutex);
	pdd = kfd_bind_process_to_device(dev, p);
	if (IS_ERR(pdd)) {
		mutex_unlock(&p->mutex);
		pr_err("Failed to get pdd\n");
		return PTR_ERR(pdd);
	}

	kfd_bo = kfd_process_device_find_bo(pdd, GET_IDR_HANDLE(handle));
	mutex_unlock(&p->mutex);

	if (!kfd_bo) {
		pr_err("Failed to get bo");
		return -EINVAL;
	}
	if (kfd_bo->kfd_ipc_obj) {
		memcpy(ipc_handle, kfd_bo->kfd_ipc_obj->share_handle,
		       sizeof(kfd_bo->kfd_ipc_obj->share_handle));
		return 0;
	}

	r = amdgpu_amdkfd_gpuvm_export_dmabuf(dev->kgd, pdd->vm,
					(struct kgd_mem *)kfd_bo->mem,
					&dmabuf);
	if (r)
		return r;

	r = ipc_store_insert(dmabuf, ipc_handle, &obj);
	if (r)
		return r;

	kfd_bo->kfd_ipc_obj = obj;

	return r;
}
