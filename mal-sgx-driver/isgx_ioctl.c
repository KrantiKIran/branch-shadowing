/*
 * (C) Copyright 2015 Intel Corporation
 *
 * Authors:
 *
 * Jarkko Sakkinen <jarkko.sakkinen@intel.com>
 * Suresh Siddha <suresh.b.siddha@intel.com>
 * Serge Ayoun <serge.ayoun@intel.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

// SL: include dummy functions for "manual" alignment
#include "isgx_dummy.h"
#include "isgx_dummy2.h"

#include "isgx.h"
#include <asm/mman.h>
#include <linux/delay.h>
#include <linux/file.h>
#include <linux/highmem.h>
#include <linux/ratelimit.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/hashtable.h>
#include <linux/shmem_fs.h>

#define barrier() __asm__ __volatile__("": : :"memory")


static void enable_lbr(void);
static void disable_lbr(void);
static void filter_lbr(void);
static void print_lbr(void);


static inline u64 __attribute__((__always_inline__))
rdtsc_beg(void)
{
    u64  tsc;
    barrier();    
    __asm __volatile("rdtscp; shl $32, %%rdx; or %%rdx, %%rax"
                     : "=a" (tsc)
                     : : "%rbx", "%rcx", "%rdx");
    barrier();
    return tsc;
}

static inline u64 __attribute__((__always_inline__))
rdtsc_end(void)
{
    u32 a, d;
    barrier();    
    __asm __volatile("rdtscp; mov %%eax, %0; mov %%edx, %1;"
                     : "=r" (a), "=r" (d)
                     : : "%rax", "%rbx", "%rcx", "%rdx");
    barrier();
    return ((u64) a) | (((u64) d) << 32);
}

// SL: function to check branch history
int isgx_ioctl_branch(void) {
	u64 beg, end;
	u32 aux=0;

	asm volatile("mov $1, %rax; mov $2, %rbx;");

	asm volatile("cmp %rax, %rbx;"); // check ei
	beg = rdtsc_beg();
	asm volatile("jne target0;");     // jump if ei == 1

    // "manual" alignment
	asm volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"); // 10
	asm volatile("nop; nop; nop; nop;"); //4
	
	asm volatile("target0:");
	asm volatile("nop; nop; nop; nop; nop;"); // 5
	asm volatile("cmp %rax, %rbx;"); // check ei
	asm volatile("jne target1;");     // jump if ei == 1

	asm volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"); // 10
	asm volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"); // 10
	asm volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"); // 10
	asm volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"); // 10
	asm volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"); // 10
	asm volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"); // 10
	asm volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"); // 10
	asm volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"); // 10

	asm volatile("target1:");
	end = rdtsc_end();
	asm volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"); // 10
	asm volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"); // 10
	asm volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"); // 10
	asm volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"); // 10
	asm volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"); // 10
	asm volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"); // 10
	asm volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"); // 10
	asm volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"); // 10

	return end-beg;
}

struct isgx_add_page_req {
	struct list_head list;
	struct isgx_enclave_page *enclave_page;
	struct isgx_secinfo secinfo;
	u64 flags;
};

static u16 isgx_isvsvnle_min = 0;
atomic_t isgx_nr_pids = ATOMIC_INIT(0);

static struct isgx_tgid_ctx *find_tgid_epc_cnt(struct pid *tgid)
{
	struct isgx_tgid_ctx *ctx;

	list_for_each_entry(ctx, &isgx_tgid_ctx_list, list)
		if (pid_nr(ctx->tgid) == pid_nr(tgid))
			return ctx;

	return NULL;
}

static int add_tgid_ctx(struct isgx_enclave *enclave)
{
	struct isgx_tgid_ctx *ctx;
	struct pid *tgid = get_pid(task_tgid(current));

	mutex_lock(&isgx_tgid_ctx_mutex);

	ctx = find_tgid_epc_cnt(tgid);
	if (ctx) {
		kref_get(&ctx->refcount);
		enclave->tgid_ctx = ctx;
		mutex_unlock(&isgx_tgid_ctx_mutex);
		put_pid(tgid);
		return 0;
	}

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		mutex_unlock(&isgx_tgid_ctx_mutex);
		put_pid(tgid);
		return -ENOMEM;
	}

	ctx->tgid = tgid;
	kref_init(&ctx->refcount);
	INIT_LIST_HEAD(&ctx->enclave_list);

	list_add(&ctx->list, &isgx_tgid_ctx_list);
	atomic_inc(&isgx_nr_pids);

	enclave->tgid_ctx = ctx;

	mutex_unlock(&isgx_tgid_ctx_mutex);
	return 0;
}
void release_tgid_ctx(struct kref *ref)
{
	struct isgx_tgid_ctx *pe =
		container_of(ref, struct isgx_tgid_ctx, refcount);
	mutex_lock(&isgx_tgid_ctx_mutex);
	list_del(&pe->list);
	atomic_dec(&isgx_nr_pids);
	mutex_unlock(&isgx_tgid_ctx_mutex);
	put_pid(pe->tgid);
	kfree(pe);
}
static int enclave_rb_insert(struct rb_root *root,
			     struct isgx_enclave_page *data)
{
	struct rb_node **new = &(root->rb_node), *parent = NULL;

	/* Figure out where to put new node */
	while (*new) {
		struct isgx_enclave_page *this =
			container_of(*new, struct isgx_enclave_page, node);

		parent = *new;
		if (data->addr < this->addr)
			new = &((*new)->rb_left);
		else if (data->addr > this->addr)
			new = &((*new)->rb_right);
		else
			return -1;
	}

	/* Add new node and rebalance tree. */
	rb_link_node(&data->node, parent, new);
	rb_insert_color(&data->node, root);

	return 0;
}

/**
 * construct_enclave_page() - populate a new enclave page instance
 * @enclave	an enclave
 * @entry	the enclave page to be populated
 * @addr	the linear address of the enclave page
 *
 * Allocates VA slot for the enclave page and fills out its fields. Returns
 * an error code on failure that can be either a POSIX error code or one of the
 * error codes defined in isgx_user.h.
 */
static int construct_enclave_page(struct isgx_enclave *enclave,
				  struct isgx_enclave_page *entry,
				  unsigned long addr)
{
	struct isgx_va_page *va_page;
	struct isgx_epc_page *epc_page = NULL;
	unsigned int va_offset = PAGE_SIZE;
	void *vaddr;
	int ret = 0;

	list_for_each_entry(va_page, &enclave->va_pages, list) {
		va_offset = isgx_alloc_va_slot(va_page);
		if (va_offset < PAGE_SIZE)
			break;
	}

	if (va_offset == PAGE_SIZE) {
		va_page = kzalloc(sizeof(*va_page), GFP_KERNEL);
		if (!va_page)
			return -ENOMEM;

		epc_page = isgx_alloc_epc_page(NULL, 0);
		if (IS_ERR(epc_page)) {
			kfree(va_page);
			return PTR_ERR(epc_page);
		}

		vaddr = isgx_get_epc_page(epc_page);
		BUG_ON(!vaddr);
		ret = __epa(vaddr);
		isgx_put_epc_page(vaddr);
		if (ret) {
			isgx_err(enclave, "EPA returned %d\n", ret);
			isgx_free_epc_page(epc_page, NULL, ISGX_FREE_EREMOVE);
			kfree(va_page);
			/* This probably a driver bug. Better to crash cleanly
			 * than let the failing driver to run.
			 */
			BUG();
		}

		va_page->epc_page = epc_page;
		va_offset = isgx_alloc_va_slot(va_page);
		list_add(&va_page->list, &enclave->va_pages);
	}

	entry->enclave = enclave;
	entry->va_page = va_page;
	entry->va_offset = va_offset;
	entry->addr = addr;

	return 0;
}

static int get_enclave(unsigned long addr, struct isgx_enclave **enclave)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	int ret;

	down_read(&mm->mmap_sem);

	ret = isgx_find_enclave(mm, addr, &vma);
	if (!ret) {
		*enclave = vma->vm_private_data;
		kref_get(&(*enclave)->refcount);
	}

	up_read(&mm->mmap_sem);

	return ret;
}

static int set_enclave(unsigned long addr, struct isgx_enclave *enclave)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	struct isgx_vma *evma;
	int ret;

	down_read(&mm->mmap_sem);

	ret = isgx_find_enclave(mm, addr, &vma);
	if (ret != -ENOENT)
		goto out;
	else
		ret = 0;

	vma->vm_private_data = enclave;

	evma = kzalloc(sizeof(struct isgx_vma), GFP_KERNEL);
	if (!evma) {
		ret = -ENOMEM;
		goto out;
	}

	evma->vma = vma;
	list_add_tail(&evma->vma_list, &enclave->vma_list);
out:
	up_read(&mm->mmap_sem);
	return ret;
}

static int validate_secs(const struct isgx_secs *secs)
{
	u32 needed_ssaframesize = 1;
	u32 tmp;
	int i;

	if (secs->flags & ISGX_SECS_A_RESERVED_MASK)
		return -EINVAL;

	if (secs->flags & ISGX_SECS_A_MODE64BIT) {
#ifdef CONFIG_X86_64
		if (secs->size > isgx_enclave_size_max_64)
			return -EINVAL;
#else
		return -EINVAL;
#endif
	} else {
		/* On 64-bit architecture allow 32-bit enclaves only in
		 * the compatibility mode.
		 */
#ifdef CONFIG_X86_64
		if (!test_thread_flag(TIF_ADDR32))
			return -EINVAL;
#endif
		if (secs->size > isgx_enclave_size_max_32)
			return -EINVAL;
	}

	if ((secs->xfrm & 0x3) != 0x3 || (secs->xfrm & ~isgx_xfrm_mask))
		return -EINVAL;

	/* SKL quirk */
	if ((secs->xfrm & BIT(3)) != (secs->xfrm & BIT(4)))
		return -EINVAL;

	for (i = 2; i < 64; i++) {
		tmp = isgx_ssaframesize_tbl[i];
		if (((1 << i) & secs->xfrm) && (tmp > needed_ssaframesize))
			needed_ssaframesize = tmp;
	}

	if (!secs->ssaframesize || !needed_ssaframesize ||
	    needed_ssaframesize > secs->ssaframesize)
		return -EINVAL;

	/* Must be power of two  */
	if (secs->size == 0 || (secs->size & (secs->size - 1)) != 0)
		return -EINVAL;

	for (i = 0; i < ISGX_SECS_RESERVED1_SIZE; i++)
		if (secs->reserved1[i])
			return -EINVAL;

	for (i = 0; i < ISGX_SECS_RESERVED2_SIZE; i++)
		if (secs->reserved2[i])
			return -EINVAL;

	for (i = 0; i < ISGX_SECS_RESERVED3_SIZE; i++)
		if (secs->reserved3[i])
			return -EINVAL;

	for (i = 0; i < ISGX_SECS_RESERVED4_SIZE; i++)
		if (secs->reserved[i])
			return -EINVAL;

	return 0;
}



static long isgx_ioctl_enclave_create(struct file *filep, unsigned int cmd,
				      unsigned long arg)
{
	struct page_info pginfo;
	struct isgx_secinfo secinfo;
	struct isgx_create_param *createp =
		(struct isgx_create_param *) arg;
	void *secs_la = createp->secs;
	struct isgx_enclave *enclave = NULL;
	struct isgx_secs *secs = NULL;
	struct isgx_epc_page *secs_epc_page;
	void *secs_vaddr = NULL;
	unsigned long backing;
	long ret;

	secs = kzalloc(sizeof(*secs),  GFP_KERNEL);
	if (!secs)
		return -ENOMEM;
	ret = copy_from_user((void *) secs, secs_la, sizeof (*secs));
	if (ret) {
		kfree(secs);
		return ret;
	}

	if (validate_secs(secs)) {
		kfree(secs);
		return -EINVAL;
	}

    // SL: Let's adjust the base address of an enclave
	//	secs->base = vm_mmap(filep, 0, secs->size,
	secs->base = vm_mmap(filep, 0x7fffc0800000, secs->size,
			     PROT_READ | PROT_WRITE | PROT_EXEC,
			     MAP_SHARED, 0);
	if (IS_ERR((void *) (unsigned long) secs->base)) {
		kfree(secs);
		pr_debug("isgx: [%d] vm_mmap() for the EPC of size 0x%lx returned %ld\n",
			 pid_nr(task_tgid(current->group_leader)),
			 (unsigned long) secs->size,
			 ret);
		return PTR_ERR((void *) (unsigned long) secs->base);
	}

	backing = vm_mmap(NULL, 0, secs->size + PAGE_SIZE,
			  PROT_READ | PROT_WRITE,
			  MAP_PRIVATE, 0);
	if (IS_ERR((void *) backing)) {
		ret = PTR_ERR((void *) backing);
		vm_munmap(secs->base, secs->size);
		kfree(secs);
		pr_debug("isgx: [%d] vm_mmap() for the backing of size 0x%lx returned %ld\n",
			 pid_nr(task_tgid(current->group_leader)),
			 (unsigned long) secs->size,
			 ret);
		return PTR_ERR((void *) backing);
	}

	enclave = kzalloc(sizeof(struct isgx_enclave), GFP_KERNEL);
	if (!enclave)
		goto out;

	kref_init(&enclave->refcount);
	INIT_LIST_HEAD(&enclave->add_page_reqs);
	INIT_LIST_HEAD(&enclave->va_pages);
	INIT_LIST_HEAD(&enclave->vma_list);
	INIT_LIST_HEAD(&enclave->load_list);
	INIT_LIST_HEAD(&enclave->enclave_list);
	mutex_init(&enclave->lock);
	INIT_WORK(&enclave->add_page_work, isgx_add_page_worker);

	enclave->owner = current->group_leader;
	enclave->mm = current->mm;
	enclave->base = secs->base;
	enclave->size = secs->size;
	enclave->backing = backing;

	ret = add_tgid_ctx(enclave);
	if (ret)
		goto out;

	secs_epc_page = isgx_alloc_epc_page(NULL, 0);
	if (IS_ERR(secs_epc_page)) {
		ret = PTR_ERR(secs_epc_page);
		secs_epc_page = NULL;
		goto out;
	}

	enclave->secs_page.epc_page = secs_epc_page;

	ret = construct_enclave_page(enclave, &enclave->secs_page,
				     enclave->base + enclave->size);
	if (ret)
		goto out;

	isgx_dbg(enclave, "ECREATE backing=0x%lx, size=0x%lx\n",
		 enclave->backing, enclave->size);

	secs_vaddr = isgx_get_epc_page(enclave->secs_page.epc_page);

	pginfo.srcpge = (unsigned long) secs;
	pginfo.linaddr = 0;
	pginfo.secinfo = (unsigned long) &secinfo;
	pginfo.secs = 0;
	memset(&secinfo, 0, sizeof(secinfo));
	ret = __ecreate((void *) &pginfo, secs_vaddr);

	isgx_put_epc_page(secs_vaddr);

	if (ret) {
		isgx_info(enclave, "ECREATE returned %d\n", ret);
		goto out;
	}

	if (secs->flags & ISGX_SECS_A_DEBUG)
		enclave->flags |= ISGX_ENCLAVE_DEBUG;

	ret = set_enclave(secs->base, enclave);

	mutex_lock(&isgx_tgid_ctx_mutex);
	list_add_tail(&enclave->enclave_list, &enclave->tgid_ctx->enclave_list);
	mutex_unlock(&isgx_tgid_ctx_mutex);
out:
	if (ret) {
		vm_munmap(secs->base, secs->size);
		vm_munmap(backing, secs->size + PAGE_SIZE);
		if (enclave)
			kref_put(&enclave->refcount, isgx_enclave_release);
	} else
		createp->addr = (unsigned long) enclave->base;

	kfree(secs);


	//	enable_lbr();
	//	filter_lbr();
	
	return ret;
}

static int validate_secinfo(struct isgx_secinfo *secinfo)
{
	u64 perm = secinfo->flags & ISGX_SECINFO_PERMISSION_MASK;
	u64 page_type = secinfo->flags & ISGX_SECINFO_PAGE_TYPE_MASK;
	int i;

	if ((secinfo->flags & ISGX_SECINFO_RESERVED_MASK) ||
	    ((perm & ISGX_SECINFO_W) && !(perm & ISGX_SECINFO_R)) ||
	    (page_type != ISGX_SECINFO_TCS && page_type != ISGX_SECINFO_REG))
		return -EINVAL;

	for (i = 0; i < sizeof(secinfo->reserved) / sizeof(u64); i++)
		if (secinfo->reserved[i])
			return -EINVAL;

	return 0;
}

static int validate_tcs(struct isgx_tcs *tcs)
{
	int i;

	/* If FLAGS is not zero, ECALL will fail. */
	if ((tcs->flags != 0) ||
	    (tcs->ossa & (PAGE_SIZE - 1)) ||
	    (tcs->ofsbase & (PAGE_SIZE - 1)) ||
	    (tcs->ogsbase & (PAGE_SIZE - 1)) ||
	    ((tcs->fslimit & 0xFFF) != 0xFFF) ||
	    ((tcs->gslimit & 0xFFF) != 0xFFF))
		return -EINVAL;

	for (i = 0; i < sizeof(tcs->reserved)/sizeof(u64); i++)
		if (tcs->reserved[i])
			return -EINVAL;

	return 0;
}

static int __enclave_add_page(struct isgx_enclave *enclave,
			      struct isgx_enclave_page *enclave_page,
			      struct isgx_add_param *addp,
			      struct isgx_secinfo *secinfo)
{
	u64 page_type = secinfo->flags & ISGX_SECINFO_PAGE_TYPE_MASK;
	struct isgx_tcs *tcs;
	struct page *backing_page;
	struct isgx_add_page_req *req = NULL;
	int ret;
	int empty;
	void *user_vaddr;
	void *tmp_vaddr;
	struct page *tmp_page;

	tmp_page = alloc_page(GFP_HIGHUSER);
	if (!tmp_page)
		return -ENOMEM;

	tmp_vaddr = kmap(tmp_page);
	ret = copy_from_user((void *) tmp_vaddr, (void *) addp->user_addr,
			     PAGE_SIZE);
	kunmap(tmp_page);
	if (ret) {
		__free_page(tmp_page);
		return -EFAULT;
	}

	if (validate_secinfo(secinfo)) {
		__free_page(tmp_page);
		return -EINVAL;
	}

	if (page_type == ISGX_SECINFO_TCS) {
		tcs = (struct isgx_tcs *) kmap(tmp_page);
		ret = validate_tcs(tcs);
		kunmap(tmp_page);
		if (ret) {
			__free_page(tmp_page);
			return ret;
		}
	}

	ret = construct_enclave_page(enclave, enclave_page, addp->addr);
	if (ret) {
		__free_page(tmp_page);
		return -EINVAL;
	}

	down_read(&enclave->mm->mmap_sem);
	mutex_lock(&enclave->lock);

	if (enclave->flags & ISGX_ENCLAVE_INITIALIZED) {
		ret = -EINVAL;
		goto out;
	}

	if (isgx_enclave_find_page(enclave, addp->addr)) {
		ret = -EEXIST;
		goto out;
	}

	if (!(req = kzalloc(sizeof(*req), GFP_KERNEL))) {
		ret = -ENOMEM;
		goto out;
	}

	backing_page = isgx_get_backing_page(enclave, enclave_page, true);
	if (IS_ERR((void *) backing_page)) {
		ret = PTR_ERR((void *) backing_page);
		goto out;
	}

	user_vaddr = kmap(backing_page);
	tmp_vaddr = kmap(tmp_page);
	memcpy(user_vaddr, tmp_vaddr, PAGE_SIZE);
	kunmap(backing_page);
	kunmap(tmp_page);

	if (page_type == ISGX_SECINFO_TCS)
		enclave_page->flags |= ISGX_ENCLAVE_PAGE_TCS;

	memcpy(&req->secinfo, secinfo, sizeof(*secinfo));

	req->enclave_page = enclave_page;
	req->flags = addp->flags;
	empty = list_empty(&enclave->add_page_reqs);
	kref_get(&enclave->refcount);
	list_add_tail(&req->list, &enclave->add_page_reqs);
	if (empty)
		queue_work(isgx_add_page_wq, &enclave->add_page_work);

	isgx_put_backing_page(backing_page, true /* write */);
out:

	if (ret) {
		kfree(req);
		isgx_free_va_slot(enclave_page->va_page,
				  enclave_page->va_offset);
	} else
		BUG_ON(enclave_rb_insert(&enclave->enclave_rb, enclave_page));

	mutex_unlock(&enclave->lock);
	up_read(&enclave->mm->mmap_sem);
	__free_page(tmp_page);
	return ret;
}

static long isgx_ioctl_enclave_add_page(struct file *filep, unsigned int cmd,
					unsigned long arg)
{
	struct isgx_add_param *addp;
	struct isgx_enclave *enclave;
	struct isgx_enclave_page *page;
	struct isgx_secinfo secinfo;
	int ret;

	addp = (struct isgx_add_param *) arg;

	if (addp->addr & (PAGE_SIZE - 1))
		return -EINVAL;

	if (copy_from_user(&secinfo, (void __user *) addp->secinfo,
			   sizeof(secinfo)))
		return -EFAULT;

	ret = get_enclave(addp->addr, &enclave);
	if (ret)
		return ret;

	if (addp->addr < enclave->base ||
	    addp->addr > (enclave->base + enclave->size - PAGE_SIZE)) {
		kref_put(&enclave->refcount, isgx_enclave_release);
		return -EINVAL;
	}

	page = kzalloc(sizeof(*page), GFP_KERNEL);
	if (!page) {
		kref_put(&enclave->refcount, isgx_enclave_release);
		return -ENOMEM;
	}

	ret = __enclave_add_page(enclave, page, addp, &secinfo);
	kref_put(&enclave->refcount, isgx_enclave_release);

	if (ret)
		kfree(page);

	return ret;
}

static int __isgx_enclave_init(struct isgx_enclave *enclave,
			       char *sigstruct,
			       struct isgx_einittoken *einittoken)
{
	int ret = ISGX_UNMASKED_EVENT;
	void *secs_va = NULL;
	int i;
	int j;

	if (einittoken->valid && einittoken->isvsvnle < isgx_isvsvnle_min)
		return ISGX_LE_ROLLBACK;

	for (i = 0; i < EINIT_TRY_COUNT; i++) {
		for (j = 0; j < EINIT_SPIN_COUNT; j++) {
			mutex_lock(&enclave->lock);
			secs_va = isgx_get_epc_page(enclave->secs_page.epc_page);
			ret = __einit(sigstruct, einittoken, secs_va);
			isgx_put_epc_page(secs_va);
			mutex_unlock(&enclave->lock);
			if (ret == ISGX_UNMASKED_EVENT)
				continue;
			else
				break;
		}

		if (ret != ISGX_UNMASKED_EVENT)
			goto out;

		msleep_interruptible(EINIT_BACKOFF_TIME);
		if (signal_pending(current))
			return -EINTR;
	}

out:
	if (ret)
		isgx_info(enclave, "EINIT returned %d\n", ret);
	else {
		enclave->flags |= ISGX_ENCLAVE_INITIALIZED;

		if (einittoken->isvsvnle > isgx_isvsvnle_min)
			isgx_isvsvnle_min = einittoken->isvsvnle;
	}

	return ret;
}

static long isgx_ioctl_enclave_init(struct file *filep, unsigned int cmd,
				    unsigned long arg)
{
	int ret = -EINVAL;
	struct isgx_init_param *initp = (struct isgx_init_param *) arg;
	unsigned long enclave_id = initp->addr;
	char *sigstruct;
	struct isgx_einittoken *einittoken;
	struct isgx_enclave *enclave;
	struct page *initp_page;

	initp_page = alloc_page(GFP_HIGHUSER);
	if (!initp_page)
		return -ENOMEM;

	sigstruct = kmap(initp_page);
	einittoken = (struct isgx_einittoken *)
		((unsigned long) sigstruct + PAGE_SIZE / 2);

	ret = copy_from_user(sigstruct, initp->sigstruct, SIGSTRUCT_SIZE);
	if (ret)
		goto out_free_page;

	ret = copy_from_user(einittoken, initp->einittoken, EINITTOKEN_SIZE);
	if (ret)
		goto out_free_page;

	ret = get_enclave(enclave_id, &enclave);
	if (ret)
		goto out_free_page;

	mutex_lock(&enclave->lock);
	if (enclave->flags & ISGX_ENCLAVE_INITIALIZED) {
		ret = -EINVAL;
		mutex_unlock(&enclave->lock);
		goto out;
	}
	mutex_unlock(&enclave->lock);

	flush_work(&enclave->add_page_work);

	ret = __isgx_enclave_init(enclave, sigstruct, einittoken);
out:
	kref_put(&enclave->refcount, isgx_enclave_release);
out_free_page:
	kunmap(initp_page);
	__free_page(initp_page);
	return ret;
}

// SL: currently, we check the branch history when an enclave has been distroyed.
static long isgx_ioctl_enclave_destroy(struct file *filep, unsigned int cmd,
				       unsigned long arg)
{
	enable_lbr();
	filter_lbr();

	int t = isgx_ioctl_branch();

	disable_lbr();
	print_lbr();
	printk("BTIME: %d\n", t);	

	// printk("isgx: %p, 0x%0x\n", isgx_ioctl_branch + 0x2e, *((char*)(isgx_ioctl_branch+0x2e)));
	// printk("isgx: %p, 0x%0x\n", isgx_ioctl_branch+0x38-0x17, *((char*)(isgx_ioctl_branch+0x38-0x17)));
	
	struct isgx_destroy_param *destroyp =
		(struct isgx_destroy_param *) arg;
	unsigned long enclave_id = destroyp->addr;
	struct isgx_enclave *enclave;
	int ret;

	ret = get_enclave(enclave_id, &enclave);
	if (ret)
		return ret;

	vm_munmap(enclave->base, enclave->size);
	vm_munmap(enclave->backing, enclave->size + PAGE_SIZE);

	kref_put(&enclave->refcount, isgx_enclave_release);
	return 0;
}

typedef long (*isgx_ioctl_t)(struct file *filep, unsigned int cmd,
			     unsigned long arg);

long isgx_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	char data[256];
	isgx_ioctl_t handler = NULL;
	long ret;

	switch (cmd) {
	case ISGX_IOCTL_ENCLAVE_CREATE:
		handler = isgx_ioctl_enclave_create;
		break;
	case ISGX_IOCTL_ENCLAVE_ADD_PAGE:
		handler = isgx_ioctl_enclave_add_page;
		break;
	case ISGX_IOCTL_ENCLAVE_INIT:
		handler = isgx_ioctl_enclave_init;
		break;
	case ISGX_IOCTL_ENCLAVE_DESTROY:
		handler = isgx_ioctl_enclave_destroy;
		break;
	default:
		return -EINVAL;
	}

	if (copy_from_user(data, (void __user *) arg, _IOC_SIZE(cmd)))
		return -EFAULT;

	ret = handler(filep, cmd, (unsigned long) ((void *) data));

	if (!ret && (cmd & IOC_OUT)) {
		if (copy_to_user((void __user *) arg, data, _IOC_SIZE(cmd)))
			return -EFAULT;
	}

	return ret;
}

static int do_eadd(struct isgx_epc_page *secs_page,
		   struct isgx_epc_page *epc_page,
		   unsigned long linaddr,
		   struct isgx_secinfo *secinfo,
		   struct page *backing_page)
{
	struct page_info pginfo;
	void *epc_page_vaddr;
	int ret;

	pginfo.srcpge = (unsigned long) kmap_atomic(backing_page);
	pginfo.secs = (unsigned long) isgx_get_epc_page(secs_page);
	epc_page_vaddr = isgx_get_epc_page(epc_page);

	pginfo.linaddr = linaddr;
	pginfo.secinfo = (unsigned long) secinfo;
	ret = __eadd(&pginfo, epc_page_vaddr);

	isgx_put_epc_page(epc_page_vaddr);
	isgx_put_epc_page((void *) (unsigned long) pginfo.secs);
	kunmap_atomic((void *) (unsigned long)  pginfo.srcpge);

	return ret;
}

static int do_eextend(struct isgx_epc_page *secs_page,
		      struct isgx_epc_page *epc_page)
{
	void *secs;
	void *epc;
	int ret = 0;
	int i;

	for (i = 0; i < 0x1000 && !ret; i += 0x100) {
		secs = isgx_get_epc_page(secs_page);
		epc = isgx_get_epc_page(epc_page);

		ret = __eextend(secs, (void *)((unsigned long) epc + i));

		isgx_put_epc_page(epc);
		isgx_put_epc_page(secs);
	}

	return ret;
}

static bool process_add_page_req(struct isgx_add_page_req *req)
{
	struct page *backing_page;
	struct isgx_epc_page *epc_page;
	struct isgx_enclave_page *enclave_page = req->enclave_page;
	unsigned int flags = req->flags;
	struct isgx_enclave *enclave = enclave_page->enclave;
	unsigned free_flags = 0;
	struct vm_area_struct *vma;
	int ret;

	epc_page = isgx_alloc_epc_page(enclave->tgid_ctx, 0);
	if (IS_ERR(epc_page))
		return false;

	if (!isgx_pin_mm(enclave)) {
		isgx_free_epc_page(epc_page, enclave, 0);
		return false;
	}

	mutex_lock(&enclave->lock);

	if (list_empty(&enclave->vma_list) ||
	    isgx_find_enclave(enclave->mm, enclave_page->addr, &vma))
		goto out;

	backing_page = isgx_get_backing_page(enclave, enclave_page,
					     false /* write */);
	if (IS_ERR(backing_page))
		goto out;

	/* Do not race with do_exit() */
	if (!atomic_read(&enclave->mm->mm_users)) {
		isgx_put_backing_page(backing_page, 0);
		goto out;
	}

	isgx_insert_pte(enclave, enclave_page, epc_page, vma);
	ret = do_eadd(enclave->secs_page.epc_page, epc_page,
		      enclave_page->addr, &req->secinfo, backing_page);

	isgx_put_backing_page(backing_page, 0);
	free_flags |= ISGX_FREE_EREMOVE;
	if (ret) {
		isgx_dbg(enclave, "EADD returned %d\n", ret);
		goto out;
	}

	enclave->secs_child_cnt++;

	if (!(flags & ISGX_ADD_SKIP_EEXTEND)) {
		ret = do_eextend(enclave->secs_page.epc_page, epc_page);
		if (ret) {
			isgx_dbg(enclave, "EEXTEND returned %d\n", ret);
			goto out;
		}
	}

	isgx_test_and_clear_young(enclave_page);

	enclave_page->epc_page = epc_page;
	list_add_tail(&enclave_page->load_list, &enclave->load_list);

	mutex_unlock(&enclave->lock);
	isgx_unpin_mm(enclave);
	return true;
out:
	isgx_free_epc_page(epc_page, enclave, free_flags);
	mutex_unlock(&enclave->lock);
	isgx_unpin_mm(enclave);
	return false;
}

void isgx_add_page_worker(struct work_struct *work)
{
	struct isgx_enclave *enclave;
	struct isgx_add_page_req *req;
	bool skip_rest = false;
	bool is_empty = false;

	enclave = container_of(work, struct isgx_enclave, add_page_work);

	do {
		schedule();

		mutex_lock(&enclave->lock);
		req = list_first_entry(&enclave->add_page_reqs,
				       struct isgx_add_page_req, list);
		list_del(&req->list);
		is_empty = list_empty(&enclave->add_page_reqs);
		mutex_unlock(&enclave->lock);

		if (!skip_rest)
			if (!process_add_page_req(req))
				skip_rest = true;

		kfree(req);
	} while (!kref_put(&enclave->refcount, isgx_enclave_release) &&
		 !is_empty);
}

static void enable_lbr(void) {
	asm volatile("xor %%edx, %%edx;"
		     "xor %%eax, %%eax;"
		     "inc %%eax;"
		     "mov $0x1d9, %%ecx;"
		     "wrmsr;"
		     : : );

	//	printk(KERN_INFO "LBR Enabled\n");
}

static void disable_lbr(void) {
	asm volatile("xor %%edx, %%edx;"
		     "xor %%eax, %%eax;"
		     "mov $0x1d9, %%ecx;"
		     "wrmsr;"
		     : : );

	//	printk(KERN_INFO "LBR Disabled\n");
}

static void filter_lbr(void) {
	asm volatile("xor %%edx, %%edx;"
		     "xor %%eax, %%eax;"
		     "mov $0x13a, %%eax;" // capture ring == 0
		     // "mov $0x139, %%eax;" // capture ring > 0
		     //		     "mov $0x1fa, %%eax;"
		     "mov $0x1c8, %%ecx;"
		     "wrmsr;"
		     : : );
}

static void print_lbr(void) {
	int ax1f, dx1f, ax1t, dx1t, msr_from_counter, msr_to_counter;
	int ax1i, dx1i, msr_lbr_info;

	printk(KERN_INFO "BSHADOW: 0x%p\n", isgx_ioctl_branch);

    // NOTE: These MSR indices are for Skylake. Not so sure about other generations (e.g., Kaby lake)
	for (msr_from_counter = 1664,msr_to_counter = 1728,msr_lbr_info=3520;
	     msr_from_counter < 1696; msr_from_counter++, msr_to_counter++, msr_lbr_info++) {
		asm volatile("mov %6, %%ecx;"
			     "rdmsr;"
			     "mov %%eax, %0;"
			     "mov %%edx, %1;"
			     "mov %7, %%ecx;"
			     "rdmsr;"
			     "mov %%eax, %2;"
			     "mov %%edx, %3;"
			     "mov %8, %%ecx;"
			     "rdmsr;"
			     "mov %%eax, %4;"
			     "mov %%edx, %5;"
			     : "=g" (ax1f), "=g" (dx1f), "=g" (ax1t), "=g" (dx1t), "=g" (ax1i), "=g" (dx1i)
			     : "g" (msr_from_counter), "g" (msr_to_counter), "g" (msr_lbr_info)
			     : "%eax", "%ecx", "%edx"
			     );

		if (ax1f) {
			printk(KERN_INFO "BSHADOW: cpu: %d, from: %8x%8x to: %8x%8x, %8x, %8x\n",
			       smp_processor_id(), dx1f, ax1f, dx1t, ax1t, dx1i, ax1i);			
		}
	}
}
