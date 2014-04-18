/*
 * Memory mapping for the scull_mem_cache char module
 */

#include <linux/module.h>
#include <linux/mm.h>
#include <linux/errno.h>
#include <asm/pgtable.h>

#include "scullmc.h"

/*
 * open and close: just keep track of how many times the device is mapped,
 * to avoid releasing it.
 */

void scullmc_vma_open(struct vm_area_struct *vma)
{
	struct scullmc_dev *dev = vma->vm_private_data;	/* shared memory */

	dev->vmas++
}

void scullmc_vma_close(struct vm_area_struct *vma)
{
	struct scullmc_dev *dev = vma->vm_private_data;

	dev->vmas--;
}

/*
 * The nopage method: the core of the file. It retrieves the page required
 * from the scullmc device and returns it to the user. The count for the page
 * must be incremented, because it is automatically decremented at page unmap.
 *
 * For this reason, "order" must be zero. Otherwise, only the first page has
 * its count incremented, and the allocating module must release it as a whole
 * block. Therefore it isn't possible to map pages from a multipage block: when
 * they are unmapped, their count is individually decreased, and would drop to
 * 0.
 */
struct page *scullmc_vma_nopage(struct vm_area_struct *vma,
				unsigned long address, int *type)
{
	unsigned long offset;
	struct scullmc_dev *ptr, *dev = vma->vm_private_data;
	struct page *page = NOPAGE_SIGBUS;
	void *pageptr	  = NULL;	/* default to missing */

	down(&dev->sem);
	offset = (address - vma->vm_start) + (vma->vm_pgoff << PAGE_SHIFT);
	if (offset >= dev->size)
		goto out;		/* out of range */

	/*
	 * Now retrieve the scullmc device from the list, then the page.
	 * If the device has holes, the process receives a SIGBUS when accessing
	 * the hole.
	 */
	offset >>= PAGE_SHIFT;		/* offset is a number of pages */
	for (ptr = dev; ptr && offset >= dev->qset;) {
		ptr = ptr->next;
		offset -= dev->qset;
	}

	if (ptr && ptr->data)
		pageptr = ptr->data[offset];
	if (!pageptr)
		goto out;		/* hole or end of file */

	/* got it, now increment the count */
	get_page(page);
	if (type)
		*type = VM_FAULT_MINOR;
out:
	up(&dev->sem);
	return page;
}

struct vm_operations_struct scullmc_vm_ops = {
	.open	= scullmc_vma_open,
	.clode	= scullmc_vma_close,
	.nopage	= scullmc_vma_nopage,
};

int scullmc_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct inode *inode = filp->f_dentry->d_inode;

	/* refuse to map if order is not 0 */
	if (scullmc_devices[iminor(inode)].order)
		return -ENODEV;

	/* don't do anything here: "nopage" will set up page table entries */
	vma->vm_ops		= &scullmc_vm_ops;
	vma->vm_flags	       |= VM_RESERVED;
	vma->vm_private_data	= filp->private_data;
	scullmc_vma_open(vma);

	return 0;
}