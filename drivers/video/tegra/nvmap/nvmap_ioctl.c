/*
 * drivers/video/tegra/nvmap/nvmap_ioctl.c
 *
 * User-space interface to nvmap
 *
 * Copyright (c) 2011-2014, NVIDIA CORPORATION. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#define pr_fmt(fmt)	"nvmap: %s() " fmt, __func__

#include <linux/dma-mapping.h>
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/nvmap.h>
#include <linux/vmalloc.h>

#include <asm/memory.h>

#include <trace/events/nvmap.h>

#include "nvmap_ioctl.h"
#include "nvmap_priv.h"

#include <linux/list.h>

static ssize_t rw_handle(struct nvmap_client *client, struct nvmap_handle *h,
			 int is_read, unsigned long h_offs,
			 unsigned long sys_addr, unsigned long h_stride,
			 unsigned long sys_stride, unsigned long elem_size,
			 unsigned long count);

static struct nvmap_handle *fd_to_handle_id(int handle)
{
	struct nvmap_handle *h;

	h = nvmap_get_id_from_dmabuf_fd(NULL, handle);
	if (!IS_ERR(h))
		return h;
	return 0;
}

static struct nvmap_handle *unmarshal_user_handle(__u32 handle)
{
	return fd_to_handle_id((int)handle);
}

struct nvmap_handle *unmarshal_user_id(u32 id)
{
	return unmarshal_user_handle(id);
}

/*
 * marshal_id/unmarshal_id are for get_id/handle_from_id.
 * These are added to support using Fd's for handle.
 */
#ifdef CONFIG_ARM64
static __u32 marshal_id(struct nvmap_handle *handle)
{
	return (__u32)((uintptr_t)handle >> 2);
}

static struct nvmap_handle *unmarshal_id(__u32 id)
{
	uintptr_t h = ((id << 2) | PAGE_OFFSET);

	return (struct nvmap_handle *)h;
}
#else
static __u32 marshal_id(struct nvmap_handle *handle)
{
	return (uintptr_t)handle;
}

static struct nvmap_handle *unmarshal_id(__u32 id)
{
	return (struct nvmap_handle *)id;
}
#endif

struct nvmap_handle *__nvmap_ref_to_id(struct nvmap_handle_ref *ref)
{
	if (!virt_addr_valid(ref))
		return 0;
	return ref->handle;
}

int nvmap_ioctl_pinop(struct file *filp, bool is_pin, void __user *arg,
		      bool is32)
{
#ifdef CONFIG_COMPAT
	struct nvmap_pin_handle_32 op32;
	__u32 __user *output32 = NULL;
#endif
	struct nvmap_pin_handle op;
	struct nvmap_handle *h;
	struct nvmap_handle *on_stack[16];
	struct nvmap_handle **refs;
	unsigned long __user *output;
	unsigned int i;
	int err = 0;

#ifdef CONFIG_COMPAT
	if (is32) {
		if (copy_from_user(&op32, arg, sizeof(op32)))
			return -EFAULT;
		op.handles = (__u32 *)(uintptr_t)op32.handles;
		op.count = op32.count;
	} else
#endif
		if (copy_from_user(&op, arg, sizeof(op)))
			return -EFAULT;

	if (!op.count)
		return -EINVAL;

	if (op.count > 1) {
		size_t bytes = op.count * sizeof(*refs); /* kcalloc below will catch overflow. */

		if (op.count > ARRAY_SIZE(on_stack))
			refs = kcalloc(op.count, sizeof(*refs), GFP_KERNEL);
		else
			refs = on_stack;

		if (!refs)
			return -ENOMEM;

		if (!access_ok(VERIFY_READ, op.handles, bytes)) {
			err = -EFAULT;
			goto out;
		}

		for (i = 0; i < op.count; i++) {
			u32 handle;
			if (__get_user(handle, &op.handles[i])) {
				err = -EFAULT;
				goto out;
			}
			refs[i] = unmarshal_user_handle(handle);
			if (!refs[i]) {
				err = -EINVAL;
				goto out;
			}
		}
	} else {
		refs = on_stack;

		/* Yes, we're storing a u32 in a pointer */
		on_stack[0] = unmarshal_user_handle((u32)(uintptr_t)op.handles);
		if (!on_stack[0]) {
			err = -EINVAL;
			goto out;
		}
	}

	trace_nvmap_ioctl_pinop(filp->private_data, is_pin, op.count, refs);
	if (is_pin)
		err = nvmap_pin_ids(filp->private_data, op.count, refs);
	else
		nvmap_unpin_ids(filp->private_data, op.count, refs);

	/* skip the output stage on unpin */
	if (err || !is_pin)
		goto out;

	/* it is guaranteed that if nvmap_pin_ids returns 0 that
	 * all of the handle_ref objects are valid, so dereferencing
	 * directly here is safe */
#ifdef CONFIG_COMPAT
	if (is32) {
		if (op.count > 1)
			output32 = (__u32 *)(uintptr_t)op.addr;
		else {
			struct nvmap_pin_handle_32 __user *tmp = arg;
			output32 = &tmp->addr;
		}

		if (!output32)
			goto out;
	} else
#endif
	{
		if (op.count > 1)
			output = op.addr;
		else {
			struct nvmap_pin_handle __user *tmp = arg;
			output = (unsigned long *)&tmp->addr;
		}

		if (!output)
			goto out;
	}

	for (i = 0; i < op.count && !err; i++) {
		unsigned long addr;

		h = refs[i];
		if (h->heap_pgalloc && h->pgalloc.contig)
			addr = page_to_phys(h->pgalloc.pages[0]);
		else if (h->heap_pgalloc)
			addr = sg_dma_address(
				((struct sg_table *)h->attachment->priv)->sgl);
		else
			addr = h->carveout->base;

#ifdef CONFIG_COMPAT
		if (is32)
			err = put_user((__u32)addr, &output32[i]);
		else
#endif
			err = put_user(addr, &output[i]);
	}

	if (err)
		nvmap_unpin_ids(filp->private_data, op.count, refs);

out:
	if (refs != on_stack)
		kfree(refs);

	return err;
}

int nvmap_ioctl_getid(struct file *filp, void __user *arg)
{
	struct nvmap_client *client = filp->private_data;
	struct nvmap_create_handle op;
	struct nvmap_handle *h = NULL;

	if (copy_from_user(&op, arg, sizeof(op)))
		return -EFAULT;

	h = unmarshal_user_handle(op.handle);
	if (!h)
		return -EINVAL;

	h = nvmap_handle_get(h);

	if (!h)
		return -EPERM;

	op.id = marshal_id(h);
	if (client == h->owner)
		h->global = true;

	nvmap_handle_put(h);

	return copy_to_user(arg, &op, sizeof(op)) ? -EFAULT : 0;
}

static int nvmap_share_release(struct inode *inode, struct file *file)
{
	struct nvmap_handle *h = file->private_data;

	nvmap_handle_put(h);
	return 0;
}

static int nvmap_share_mmap(struct file *file, struct vm_area_struct *vma)
{
	/* unsupported operation */
	WARN(1, "mmap is not supported on fd, which shares nvmap handle");
	return -EPERM;
}

const struct file_operations nvmap_fd_fops = {
	.owner		= THIS_MODULE,
	.release	= nvmap_share_release,
	.mmap		= nvmap_share_mmap,
};

int nvmap_ioctl_getfd(struct file *filp, void __user *arg)
{
	struct nvmap_handle *handle;
	struct nvmap_create_handle op;
	struct nvmap_client *client = filp->private_data;

	if (copy_from_user(&op, arg, sizeof(op)))
		return -EFAULT;

	handle = unmarshal_user_handle(op.handle);
	if (!handle)
		return -EINVAL;

	op.fd = nvmap_get_dmabuf_fd(client, handle);
	if (op.fd < 0)
		return op.fd;

	if (copy_to_user(arg, &op, sizeof(op))) {
		sys_close(op.fd);
		return -EFAULT;
	}
	return 0;
}

int nvmap_ioctl_alloc(struct file *filp, void __user *arg)
{
	struct nvmap_alloc_handle op;
	struct nvmap_client *client = filp->private_data;
	struct nvmap_handle *handle;

	if (copy_from_user(&op, arg, sizeof(op)))
		return -EFAULT;

	handle = unmarshal_user_handle(op.handle);
	if (!handle)
		return -EINVAL;

	if (op.align & (op.align - 1))
		return -EINVAL;

	/* user-space handles are aligned to page boundaries, to prevent
	 * data leakage. */
	op.align = max_t(size_t, op.align, PAGE_SIZE);
#if defined(CONFIG_NVMAP_FORCE_ZEROED_USER_PAGES)
	op.flags |= NVMAP_HANDLE_ZEROED_PAGES;
#endif

	return nvmap_alloc_handle(client, handle, op.heap_mask, op.align,
				  0, /* no kind */
				  op.flags & (~NVMAP_HANDLE_KIND_SPECIFIED));
}

int nvmap_ioctl_alloc_kind(struct file *filp, void __user *arg)
{
	struct nvmap_alloc_kind_handle op;
	struct nvmap_client *client = filp->private_data;
	struct nvmap_handle *handle;

	if (copy_from_user(&op, arg, sizeof(op)))
		return -EFAULT;

	handle = unmarshal_user_handle(op.handle);
	if (!handle)
		return -EINVAL;

	if (op.align & (op.align - 1))
		return -EINVAL;

	/* user-space handles are aligned to page boundaries, to prevent
	 * data leakage. */
	op.align = max_t(size_t, op.align, PAGE_SIZE);
#if defined(CONFIG_NVMAP_FORCE_ZEROED_USER_PAGES)
	op.flags |= NVMAP_HANDLE_ZEROED_PAGES;
#endif

	return nvmap_alloc_handle(client, handle,
				  op.heap_mask,
				  op.align,
				  op.kind,
				  op.flags);
}

int nvmap_create_fd(struct nvmap_handle *h)
{
	int fd;

	fd = __nvmap_dmabuf_fd(h->dmabuf, O_CLOEXEC);
	BUG_ON(fd == 0);
	if (fd < 0) {
		pr_err("Out of file descriptors");
		return fd;
	}
	/* __nvmap_dmabuf_fd() associates fd with dma_buf->file *.
	 * fd close drops one ref count on dmabuf->file *.
	 * to balance ref count, ref count dma_buf.
	 */
	get_dma_buf(h->dmabuf);
	return fd;
}

int nvmap_ioctl_create(struct file *filp, unsigned int cmd, void __user *arg)
{
	struct nvmap_create_handle op;
	struct nvmap_handle_ref *ref = NULL;
	struct nvmap_client *client = filp->private_data;
	int err = 0;
	int fd = 0;

	if (copy_from_user(&op, arg, sizeof(op)))
		return -EFAULT;

	if (!client)
		return -ENODEV;

	if (cmd == NVMAP_IOC_CREATE) {
		ref = nvmap_create_handle(client, PAGE_ALIGN(op.size));
		if (!IS_ERR(ref))
			ref->handle->orig_size = op.size;
	} else if (cmd == NVMAP_IOC_FROM_ID) {
		ref = nvmap_duplicate_handle(client, unmarshal_id(op.id), 0);
	} else if (cmd == NVMAP_IOC_FROM_FD) {
		ref = nvmap_create_handle_from_fd(client, op.fd);
	} else {
		return -EINVAL;
	}

	if (IS_ERR(ref))
		return PTR_ERR(ref);

	fd = nvmap_create_fd(ref->handle);
	if (fd < 0)
		err = fd;

	op.handle = fd;

	if (copy_to_user(arg, &op, sizeof(op))) {
		err = -EFAULT;
		nvmap_free_handle(client, __nvmap_ref_to_id(ref));
	}

	if (err && fd > 0)
		sys_close(fd);
	return err;
}

int nvmap_map_into_caller_ptr(struct file *filp, void __user *arg, bool is32)
{
	struct nvmap_client *client = filp->private_data;
	struct nvmap_map_caller op;
#ifdef CONFIG_COMPAT
	struct nvmap_map_caller_32 op32;
#endif
	struct nvmap_vma_priv *vpriv;
	struct vm_area_struct *vma;
	struct nvmap_handle *h = NULL;
	int err = 0;

#ifdef CONFIG_COMPAT
	if (is32) {
		if (copy_from_user(&op32, arg, sizeof(op32)))
			return -EFAULT;
		op.handle = op32.handle;
		op.offset = op32.offset;
		op.length = op32.length;
		op.flags = op32.length;
		op.addr = op32.addr;
	} else
#endif
		if (copy_from_user(&op, arg, sizeof(op)))
			return -EFAULT;

	h = unmarshal_user_handle(op.handle);

	if (!h)
		return -EINVAL;

	h = nvmap_handle_get(h);

	if (!h)
		return -EPERM;

	if(!h->alloc) {
		nvmap_handle_put(h);
		return -EFAULT;
	}

	trace_nvmap_map_into_caller_ptr(client, h, op.offset,
					op.length, op.flags);
	down_read(&current->mm->mmap_sem);

	vma = find_vma(current->mm, op.addr);
	if (!vma || !vma->vm_private_data) {
		err = -ENOMEM;
		goto out;
	}

	if (op.offset & ~PAGE_MASK) {
		err = -EFAULT;
		goto out;
	}

	if (op.offset >= h->size || op.length > h->size - op.offset) {
		err = -EADDRNOTAVAIL;
		goto out;
	}

	vpriv = vma->vm_private_data;
	BUG_ON(!vpriv);

	/* the VMA must exactly match the requested mapping operation, and the
	 * VMA that is targetted must have been created by this driver
	 */
	if ((vma->vm_start != op.addr) || !is_nvmap_vma(vma) ||
	    (vma->vm_end-vma->vm_start != op.length)) {
		err = -EPERM;
		goto out;
	}

	/* verify that each mmap() system call creates a unique VMA */

	if (vpriv->handle && (h == vpriv->handle)) {
		goto out;
	} else if (vpriv->handle) {
		err = -EADDRNOTAVAIL;
		goto out;
	}

	if (!h->heap_pgalloc && (h->carveout->base & ~PAGE_MASK)) {
		err = -EFAULT;
		goto out;
	}

	vma->vm_flags |= (h->heap_pgalloc ? 0 : VM_PFNMAP);
	vpriv->handle = h;
	vpriv->offs = op.offset;
	vma->vm_page_prot = nvmap_pgprot(h, vma->vm_page_prot);
	nvmap_vma_open(vma);

out:
	up_read(&current->mm->mmap_sem);

	if (err)
		nvmap_handle_put(h);
	return err;
}

int nvmap_ioctl_get_param(struct file *filp, void __user *arg, bool is32)
{
#ifdef CONFIG_COMPAT
	struct nvmap_handle_param_32 __user *uarg32 = arg;
#endif
	struct nvmap_handle_param __user *uarg = arg;
	struct nvmap_handle_param op;
	struct nvmap_client *client = filp->private_data;
	struct nvmap_handle_ref *ref;
	struct nvmap_handle *h;
	u64 result;
	int err = 0;

#ifdef CONFIG_COMPAT
	/* This is safe because the incoming value of result doesn't matter */
	if (is32) {
		if (copy_from_user(&op, arg,
				sizeof(struct nvmap_handle_param_32)))
			return -EFAULT;
	} else
#endif
		if (copy_from_user(&op, arg, sizeof(op)))
			return -EFAULT;

	h = unmarshal_user_handle(op.handle);
	if (!h)
		return -EINVAL;

	h = nvmap_handle_get(h);
	if (!h)
		return -EINVAL;

	nvmap_ref_lock(client);
	ref = __nvmap_validate_locked(client, h);
	if (IS_ERR_OR_NULL(ref)) {
		err = ref ? PTR_ERR(ref) : -EINVAL;
		goto ref_fail;
	}

	err = nvmap_get_handle_param(client, ref, op.param, &result);

#ifdef CONFIG_COMPAT
	if (is32)
		err = put_user((__u32)result, &uarg32->result);
	else
#endif
		err = put_user((unsigned long)result, &uarg->result);

ref_fail:
	nvmap_ref_unlock(client);
	nvmap_handle_put(h);
	return err;
}

int nvmap_ioctl_rw_handle(struct file *filp, int is_read, void __user *arg,
			  bool is32)
{
	struct nvmap_client *client = filp->private_data;
	struct nvmap_rw_handle __user *uarg = arg;
	struct nvmap_rw_handle op;
#ifdef CONFIG_COMPAT
	struct nvmap_rw_handle_32 __user *uarg32 = arg;
	struct nvmap_rw_handle_32 op32;
#endif
	struct nvmap_handle *h;
	ssize_t copied;
	int err = 0;

#ifdef CONFIG_COMPAT
	if (is32) {
		if (copy_from_user(&op32, arg, sizeof(op32)))
			return -EFAULT;
		op.addr = op32.addr;
		op.handle = op32.handle;
		op.offset = op32.offset;
		op.elem_size = op32.elem_size;
		op.hmem_stride = op32.hmem_stride;
		op.user_stride = op32.user_stride;
		op.count = op32.count;
	} else
#endif
		if (copy_from_user(&op, arg, sizeof(op)))
			return -EFAULT;

	h = unmarshal_user_handle(op.handle);
	if (!h || !op.addr || !op.count || !op.elem_size)
		return -EINVAL;

	h = nvmap_handle_get(h);
	if (!h)
		return -EPERM;

	trace_nvmap_ioctl_rw_handle(client, h, is_read, op.offset,
				    op.addr, op.hmem_stride,
				    op.user_stride, op.elem_size, op.count);
	copied = rw_handle(client, h, is_read, op.offset,
			   (unsigned long)op.addr, op.hmem_stride,
			   op.user_stride, op.elem_size, op.count);

	if (copied < 0) {
		err = copied;
		copied = 0;
	} else if (copied < (op.count * op.elem_size))
		err = -EINTR;

#ifdef CONFIG_COMPAT
	if (is32)
		__put_user(copied, &uarg32->count);
	else
#endif
		__put_user(copied, &uarg->count);

	nvmap_handle_put(h);

	return err;
}

static int __nvmap_cache_maint(struct nvmap_client *client,
			       struct nvmap_cache_op *op)
{
	struct vm_area_struct *vma;
	struct nvmap_vma_priv *vpriv;
	struct nvmap_handle *handle;
	unsigned long start;
	unsigned long end;
	int err = 0;

	handle = unmarshal_user_handle(op->handle);
	if (!handle || !op->addr || op->op < NVMAP_CACHE_OP_WB ||
	    op->op > NVMAP_CACHE_OP_WB_INV)
		return -EINVAL;

	down_read(&current->mm->mmap_sem);

	vma = find_vma(current->active_mm, (unsigned long)op->addr);
	if (!vma || !is_nvmap_vma(vma) ||
	    (ulong)op->addr < vma->vm_start ||
	    (ulong)op->addr >= vma->vm_end ||
	    op->len > vma->vm_end - (ulong)op->addr) {
		err = -EADDRNOTAVAIL;
		goto out;
	}

	vpriv = (struct nvmap_vma_priv *)vma->vm_private_data;

	if (vpriv->handle != handle) {
		err = -EFAULT;
		goto out;
	}

	start = (unsigned long)op->addr - vma->vm_start +
		(vma->vm_pgoff << PAGE_SHIFT);
	end = start + op->len;

	err = __nvmap_do_cache_maint(client, vpriv->handle, start, end, op->op,
				     false);
out:
	up_read(&current->mm->mmap_sem);
	return err;
}

int nvmap_ioctl_cache_maint(struct file *filp, void __user *arg, bool is32)
{
	struct nvmap_client *client = filp->private_data;
	struct nvmap_cache_op op;
#ifdef CONFIG_COMPAT
	struct nvmap_cache_op_32 op32;
#endif

#ifdef CONFIG_COMPAT
	if (is32) {
		if (copy_from_user(&op32, arg, sizeof(op32)))
			return -EFAULT;
		op.addr = op32.addr;
		op.handle = op32.handle;
		op.len = op32.len;
		op.op = op32.op;
	} else
#endif
		if (copy_from_user(&op, arg, sizeof(op)))
			return -EFAULT;

	return __nvmap_cache_maint(client, &op);
}

int nvmap_ioctl_free(struct file *filp, unsigned long arg)
{
	struct nvmap_client *client = filp->private_data;

	if (!arg)
		return 0;

	nvmap_free_handle_user_id(client, arg);
	return sys_close(arg);
}

static void inner_cache_maint(unsigned int op, void *vaddr, size_t size)
{
	if (op == NVMAP_CACHE_OP_WB_INV)
		dmac_flush_range(vaddr, vaddr + size);
	else if (op == NVMAP_CACHE_OP_INV)
		dmac_map_area(vaddr, size, DMA_FROM_DEVICE);
	else
		dmac_map_area(vaddr, size, DMA_TO_DEVICE);
}

static void outer_cache_maint(unsigned int op, phys_addr_t paddr, size_t size)
{
	if (op == NVMAP_CACHE_OP_WB_INV)
		outer_flush_range(paddr, paddr + size);
	else if (op == NVMAP_CACHE_OP_INV)
		outer_inv_range(paddr, paddr + size);
	else
		outer_clean_range(paddr, paddr + size);
}

static void heap_page_cache_maint(
	struct nvmap_handle *h, unsigned long start, unsigned long end,
	unsigned int op, bool inner, bool outer, pte_t **pte,
	unsigned long kaddr, pgprot_t prot, bool clean_only_dirty)
{
	if (h->userflags & NVMAP_HANDLE_CACHE_SYNC) {
		/*
		 * zap user VA->PA mappings so that any access to the pages
		 * will result in a fault and can be marked dirty
		 */
		nvmap_handle_mkclean(h, start, end-start);
		nvmap_zap_handle(h, start, end - start);
	}

#ifdef NVMAP_LAZY_VFREE
	if (inner) {
		void *vaddr = NULL;

		if (!h->vaddr) {
			struct page **pages;
			pages = nvmap_pages(h->pgalloc.pages,
					    h->size >> PAGE_SHIFT);
			if (!pages)
				goto per_page_cache_maint;
			vaddr = vm_map_ram(pages,
					h->size >> PAGE_SHIFT, -1, prot);
			nvmap_altfree(pages,
				(h->size >> PAGE_SHIFT) * sizeof(*pages));
		}
		if (vaddr && atomic_long_cmpxchg(&h->vaddr, 0, (long)vaddr))
			vm_unmap_ram(vaddr, h->size >> PAGE_SHIFT);
		if (h->vaddr) {
			/* Fast inner cache maintenance using single mapping */
			inner_cache_maint(op, h->vaddr + start, end - start);
			if (!outer)
				return;
			/* Skip per-page inner maintenance in loop below */
			inner = false;
		}
	}
per_page_cache_maint:
#endif

	while (start < end) {
		struct page *page;
		phys_addr_t paddr;
		unsigned long next;
		unsigned long off;
		size_t size;

		page = nvmap_to_page(h->pgalloc.pages[start >> PAGE_SHIFT]);
		next = min(((start + PAGE_SIZE) & PAGE_MASK), end);
		off = start & ~PAGE_MASK;
		size = next - start;
		paddr = page_to_phys(page) + off;

		if (inner) {
			void *vaddr = (void *)kaddr + off;
			BUG_ON(!pte);
			BUG_ON(!kaddr);
			set_pte_at(&init_mm, kaddr, *pte,
				pfn_pte(__phys_to_pfn(paddr), prot));
			nvmap_flush_tlb_kernel_page(kaddr);
			inner_cache_maint(op, vaddr, size);
		}

		if (outer)
			outer_cache_maint(op, paddr, size);
		start = next;
	}
}

#if defined(CONFIG_NVMAP_OUTER_CACHE_MAINT_BY_SET_WAYS)
static bool fast_cache_maint_outer(unsigned long start,
		unsigned long end, unsigned int op)
{
	bool result = false;
	if (end - start >= cache_maint_outer_threshold) {
		if (op == NVMAP_CACHE_OP_WB_INV) {
			outer_flush_all();
			result = true;
		}
		if (op == NVMAP_CACHE_OP_WB) {
			outer_clean_all();
			result = true;
		}
	}

	return result;
}
#else
static inline bool fast_cache_maint_outer(unsigned long start,
		unsigned long end, unsigned int op)
{
	return false;
}
#endif

#if defined(CONFIG_NVMAP_CACHE_MAINT_BY_SET_WAYS)
static inline bool can_fast_cache_maint(struct nvmap_handle *h,
	unsigned long start,
	unsigned long end, unsigned int op)
{
	if ((op == NVMAP_CACHE_OP_INV) ||
		((end - start) < cache_maint_inner_threshold))
		return false;
	return true;
}
#else
static inline bool can_fast_cache_maint(struct nvmap_handle *h,
	unsigned long start,
	unsigned long end, unsigned int op)
{
	return false;
}
#endif

static bool fast_cache_maint(struct nvmap_handle *h,
	unsigned long start,
	unsigned long end, unsigned int op,
	bool clean_only_dirty)
{
	if (!can_fast_cache_maint(h, start, end, op))
		return false;

	if (h->userflags & NVMAP_HANDLE_CACHE_SYNC) {
		nvmap_handle_mkclean(h, 0, h->size);
		nvmap_zap_handle(h, 0, h->size);
	}

	if (op == NVMAP_CACHE_OP_WB_INV)
		inner_flush_cache_all();
	else if (op == NVMAP_CACHE_OP_WB)
		inner_clean_cache_all();

	/* outer maintenance */
	if (h->flags != NVMAP_HANDLE_INNER_CACHEABLE) {
		if(!fast_cache_maint_outer(start, end, op))
		{
			if (h->heap_pgalloc) {
				heap_page_cache_maint(h, start,
					end, op, false, true, NULL, 0, 0,
					clean_only_dirty);
			} else  {
				phys_addr_t pstart;

				pstart = start + h->carveout->base;
				outer_cache_maint(op, pstart, end - start);
			}
		}
	}
	return true;
}

struct cache_maint_op {
	phys_addr_t start;
	phys_addr_t end;
	unsigned int op;
	struct nvmap_handle *h;
	bool inner;
	bool outer;
	bool clean_only_dirty;
};

static int do_cache_maint(struct cache_maint_op *cache_work)
{
	pgprot_t prot;
	pte_t **pte = NULL;
	unsigned long kaddr;
	phys_addr_t pstart = cache_work->start;
	phys_addr_t pend = cache_work->end;
	phys_addr_t loop;
	int err = 0;
	struct nvmap_handle *h = cache_work->h;
	struct nvmap_client *client;
	unsigned int op = cache_work->op;

	if (!h || !h->alloc)
		return -EFAULT;

	client = h->owner;
	if (can_fast_cache_maint(h, pstart, pend, op))
		nvmap_stats_inc(NS_CFLUSH_DONE, cache_maint_inner_threshold);
	else
		nvmap_stats_inc(NS_CFLUSH_DONE, pend - pstart);
	trace_nvmap_cache_maint(client, h, pstart, pend, op, pend - pstart);
	trace_nvmap_cache_flush(pend - pstart,
		nvmap_stats_read(NS_ALLOC),
		nvmap_stats_read(NS_CFLUSH_RQ),
		nvmap_stats_read(NS_CFLUSH_DONE));

	wmb();
	if (h->flags == NVMAP_HANDLE_UNCACHEABLE ||
	    h->flags == NVMAP_HANDLE_WRITE_COMBINE || pstart == pend)
		goto out;

	if (fast_cache_maint(h, pstart, pend, op, cache_work->clean_only_dirty))
		goto out;

	prot = nvmap_pgprot(h, PG_PROT_KERNEL);
	pte = nvmap_alloc_pte(h->dev, (void **)&kaddr);
	if (IS_ERR(pte)) {
		err = PTR_ERR(pte);
		pte = NULL;
		goto out;
	}

	if (h->heap_pgalloc) {
		heap_page_cache_maint(h, pstart, pend, op, true,
			(h->flags == NVMAP_HANDLE_INNER_CACHEABLE) ?
					false : true,
			pte, kaddr, prot,
			cache_work->clean_only_dirty);
		goto out;
	}

	if (pstart > h->size || pend > h->size) {
		pr_warn("cache maintenance outside handle\n");
		err = -EINVAL;
		goto out;
	}

	pstart += h->carveout->base;
	pend += h->carveout->base;
	loop = pstart;

	while (loop < pend) {
		phys_addr_t next = (loop + PAGE_SIZE) & PAGE_MASK;
		void *base = (void *)kaddr + (loop & ~PAGE_MASK);
		next = min(next, pend);

		set_pte_at(&init_mm, kaddr, *pte,
			   pfn_pte(__phys_to_pfn(loop), prot));
		nvmap_flush_tlb_kernel_page(kaddr);

		inner_cache_maint(op, base, next - loop);
		loop = next;
	}

	if (h->flags != NVMAP_HANDLE_INNER_CACHEABLE)
		outer_cache_maint(op, pstart, pend - pstart);

out:
	if (pte)
		nvmap_free_pte(h->dev, pte);
	return err;
}

int __nvmap_do_cache_maint(struct nvmap_client *client,
			struct nvmap_handle *h,
			unsigned long start, unsigned long end,
			unsigned int op, bool clean_only_dirty)
{
	int err;
	struct cache_maint_op cache_op;

	h = nvmap_handle_get(h);
	if (!h)
		return -EFAULT;

	if (op == NVMAP_CACHE_OP_INV)
		op = NVMAP_CACHE_OP_WB_INV;

	/* clean only dirty is applicable only for Write Back operation */
	if (op != NVMAP_CACHE_OP_WB)
		clean_only_dirty = false;

	cache_op.h = h;
	cache_op.start = start;
	cache_op.end = end;
	cache_op.op = op;
	cache_op.inner = h->flags == NVMAP_HANDLE_CACHEABLE ||
			 h->flags == NVMAP_HANDLE_INNER_CACHEABLE;
	cache_op.outer = h->flags == NVMAP_HANDLE_CACHEABLE;
	cache_op.clean_only_dirty = clean_only_dirty;

	nvmap_stats_inc(NS_CFLUSH_RQ, end - start);
	err = do_cache_maint(&cache_op);
	nvmap_handle_put(h);
	return err;
}

static int rw_handle_page(struct nvmap_handle *h, int is_read,
			  unsigned long start, unsigned long rw_addr,
			  unsigned long bytes, unsigned long kaddr, pte_t *pte)
{
	pgprot_t prot = nvmap_pgprot(h, PG_PROT_KERNEL);
	unsigned long end = start + bytes;
	int err = 0;

	while (!err && start < end) {
		struct page *page = NULL;
		phys_addr_t phys;
		size_t count;
		void *src;

		if (!h->heap_pgalloc) {
			phys = h->carveout->base + start;
		} else {
			page =
			   nvmap_to_page(h->pgalloc.pages[start >> PAGE_SHIFT]);
			BUG_ON(!page);
			get_page(page);
			phys = page_to_phys(page) + (start & ~PAGE_MASK);
		}

		set_pte_at(&init_mm, kaddr, pte,
			   pfn_pte(__phys_to_pfn(phys), prot));
		nvmap_flush_tlb_kernel_page(kaddr);

		src = (void *)kaddr + (phys & ~PAGE_MASK);
		phys = PAGE_SIZE - (phys & ~PAGE_MASK);
		count = min_t(size_t, end - start, phys);

		if (is_read)
			err = copy_to_user((void *)rw_addr, src, count);
		else
			err = copy_from_user(src, (void *)rw_addr, count);

		if (err)
			err = -EFAULT;

		rw_addr += count;
		start += count;

		if (page)
			put_page(page);
	}

	return err;
}

static ssize_t rw_handle(struct nvmap_client *client, struct nvmap_handle *h,
			 int is_read, unsigned long h_offs,
			 unsigned long sys_addr, unsigned long h_stride,
			 unsigned long sys_stride, unsigned long elem_size,
			 unsigned long count)
{
	ssize_t copied = 0;
	pte_t **pte;
	void *addr;
	int ret = 0;

	if (!elem_size)
		return -EINVAL;

	if (!h->alloc)
		return -EFAULT;

	if (elem_size == h_stride && elem_size == sys_stride) {
		elem_size *= count;
		h_stride = elem_size;
		sys_stride = elem_size;
		count = 1;
	}

	pte = nvmap_alloc_pte(nvmap_dev, &addr);
	if (IS_ERR(pte))
		return PTR_ERR(pte);

	while (count--) {
		if (h_offs + elem_size > h->size) {
			nvmap_warn(client, "read/write outside of handle\n");
			ret = -EFAULT;
			break;
		}
		if (is_read)
			__nvmap_do_cache_maint(client, h, h_offs,
				h_offs + elem_size, NVMAP_CACHE_OP_INV, false);

		ret = rw_handle_page(h, is_read, h_offs, sys_addr,
				     elem_size, (unsigned long)addr, *pte);

		if (ret)
			break;

		if (!is_read)
			__nvmap_do_cache_maint(client, h, h_offs,
				h_offs + elem_size, NVMAP_CACHE_OP_WB_INV,
				false);

		copied += elem_size;
		sys_addr += sys_stride;
		h_offs += h_stride;
	}

	nvmap_free_pte(nvmap_dev, pte);
	return ret ?: copied;
}

int nvmap_ioctl_cache_maint_list(struct file *filp, void __user *arg,
				 bool is_reserve_ioctl)
{
	struct nvmap_cache_op_list op;
	u32 *handle_ptr;
	u32 *offset_ptr;
	u32 *size_ptr;
	struct nvmap_handle **refs;
	int i, err = 0;

	if (copy_from_user(&op, arg, sizeof(op)))
		return -EFAULT;

	if (!op.nr)
		return -EINVAL;

	if (!access_ok(VERIFY_READ, op.handles, op.nr * sizeof(u32)))
		return -EFAULT;

	if (!access_ok(VERIFY_READ, op.offsets, op.nr * sizeof(u32)))
		return -EFAULT;

	if (!access_ok(VERIFY_READ, op.sizes, op.nr * sizeof(u32)))
		return -EFAULT;

	if (!op.offsets || !op.sizes)
		return -EINVAL;

	refs = kcalloc(op.nr, sizeof(*refs), GFP_KERNEL);

	if (!refs)
		return -ENOMEM;

	handle_ptr = (u32 *)(uintptr_t)op.handles;
	offset_ptr = (u32 *)(uintptr_t)op.offsets;
	size_ptr = (u32 *)(uintptr_t)op.sizes;

	for (i = 0; i < op.nr; i++) {
		u32 handle;

		if (copy_from_user(&handle, &handle_ptr[i], sizeof(handle))) {
			err = -EFAULT;
			goto free_mem;
		}

		refs[i] = unmarshal_user_handle(handle);
		if (!refs[i]) {
			err = -EINVAL;
			goto free_mem;
		}
	}

	if (is_reserve_ioctl)
		err = nvmap_reserve_pages(refs, offset_ptr, size_ptr,
					  op.nr, op.op);
	else
		err = nvmap_do_cache_maint_list(refs, offset_ptr, size_ptr,
						op.op, op.nr);

free_mem:
	kfree(refs);
	return err;
}

