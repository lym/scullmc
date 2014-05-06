/*
 * scullmc is a cut-down version of the *scull* module that
 * implements only the bare device - the persistent memory region. Unlike scull,
 * which uses `kmalloc`, scull_mem_cache uses memory caches. The size of the
 * quantum can be modified at compile time and at load time, but not at
 * runtime - that would require creating a new memory cache, that we don't
 * want to get into right now.
 *
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/proc_fs.h>
#include <linux/fcntl.h>
#include <linux/aio.h>
#include <linux/semaphore.h>
#include <asm/uaccess.h>

#include "scullmc.h"

int scullmc_major	= SCULLMC_MAJOR;
int scullmc_devs	= SCULLMC_DEVS;
int scullmc_qset	= SCULLMC_QSET;
int scullmc_quantum	= SCULLMC_QUANTUM;

module_param(scullmc_major, int, 0);
module_param(scullmc_devs, int, 0);
module_param(scullmc_qset, int, 0);
module_param(scullmc_quantum, int, 0);

MODULE_AUTHOR("Salym Senyonga");
MODULE_LICENSE("GPL");

struct scullmc_dev *scullmc_devices;	/* allocated in scullmc_init */

int scullmc_trim(struct scullmc_dev *dev);
void scullmc_cleanup(void);

/* declare one cache pointer; use it for all devices */
struct kmem_cache *scullmc_cache;

#ifdef SCULLMC_USE_PROC		/* don't waste space if unused */
/*
 * use seq_file interface here
 *
 * To be Added later after all other code written
 */
#endif	/* SCULLMC_USE_PROC */

/* open and close */
int scullmc_open(struct inode *inode, struct file *filp)
{
	struct scullmc_dev *dev;		/* device information */

	/* find the device */
	dev = container_of(inode->i_cdev, struct scullmc_dev, cdev);

	/* now trim to 0 the length of the device if open was write-only */
	if ((filp->f_flags & O_ACCMODE) == O_WRONLY) {
		if (down_interruptible(&dev->sem))
			return -ERESTARTSYS;
		scullmc_trim(dev);
		up(&dev->sem);
	}

	/* and use filp->private_data to point to the device data */
	filp->private_data = dev;

	return 0;
}

int scullmc_release(struct inode *inode, struct file *filp)
{
	return 0;
}

/* Follow the list */
struct scullmc_dev *scullmc_follow(struct scullmc_dev *dev, int n)
{
	while (n--) {
		if (!dev->next) {
			dev->next = kmalloc(sizeof(struct scullmc_dev), GFP_KERNEL);
			memset(dev->next, 0, sizeof(struct scullmc_dev));
		}
		dev = dev->next;
		continue;
	}
	return dev;
}

/* Data management: read and write */
ssize_t scullmc_read(struct file *filp, char __user *buf, size_t count,
		     loff_t *f_pos)
{
	int item, s_pos, q_pos, rest;
	struct scullmc_dev *dptr;
	struct scullmc_dev *dev	= filp->private_data;	/* the first list-item */
	int quantum		= dev->quantum;
	int qset		= dev->qset;
	int itemsize		= quantum * qset;	/* how many bytes in the list item */
	ssize_t retval		= 0;

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;
	if (*f_pos > dev->size)
		goto nothing;
	if (*f_pos + count > dev->size)
		count = dev->size - *f_pos;

	/* find list-item, qset and offset in the quantum */
	item  = ((long) *f_pos) / itemsize;
	rest  = ((long) *f_pos) % itemsize;
	s_pos = rest / quantum;
	q_pos = rest % quantum;

	/* follow the list up to the right position (defined elsewhere) */
	dptr = scullmc_follow(dev, item);

	if (!dptr->data)
		goto nothing;
	if (!dptr->data[s_pos])
		goto nothing;
	if (count > quantum - q_pos)
		count	= quantum - q_pos;	/* read only up to the end of this quantum */

	if (copy_to_user(buf, dptr->data[s_pos] + q_pos, count)) {
		retval	= -EFAULT;
		goto nothing;
	}
	up(&dev->sem);

	*f_pos += count;
	return count;

nothing:
	up(&dev->sem);
	return retval;
}

ssize_t scullmc_write(struct file *filp, const char __user *buf, size_t count,
		      loff_t *f_pos)
{
	struct scullmc_dev *dptr;
	int item, s_pos, q_pos, rest;
	struct scullmc_dev *dev	= filp->private_data;
	int quantum		= dev->quantum;
	int qset		= dev->qset;
	int itemsize		= quantum * qset;
	ssize_t retval		= -ENOMEM;

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;

	/* find the list-item, qset index and offset in the quantum */
	item	= ((long) *f_pos) / itemsize;
	rest	= ((long) *f_pos) % itemsize;
	s_pos	= rest / quantum;
	q_pos	= rest % quantum;

	/* follow the list up to the right position */
	dptr	= scullmc_follow(dev, item);
	if (!dptr->data) {
		dptr->data = kmalloc(qset * sizeof(void *), GFP_KERNEL);
		if (!dptr->data)
			goto nomem;
		memset(dptr->data, 0, qset * sizeof(char *));
	}

	/* Allocate a quantum using memory cache */
	if (!dptr->data[s_pos]) {
		dptr->data[s_pos] = kmem_cache_alloc(scullmc_cache, GFP_KERNEL);
		if (!dptr->data[s_pos])
			goto nomem;
		memset(dptr->data[s_pos], 0, scullmc_quantum);
	}

	if (count > quantum - q_pos)
		count = quantum - q_pos;	/* write only upto the end of this quantum */
	if (copy_from_user(dptr->data[s_pos] + q_pos, buf, count)) {
		retval	= -EFAULT;
		goto nomem;
	}
	*f_pos += count;

	/* update the size */
	if (dev->size < *f_pos)
		dev->size = *f_pos;
	up(&dev->sem);
	return count;

nomem:
	up(&dev->sem);
	return retval;
}

/* The ioctl implementation */
long scullmc_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int tmp;
	int err	= 0;
	int ret	= 0;

	/* don't decode wrong cmds */
	if (_IOC_TYPE(cmd) != SCULLMC_IOC_MAGIC)
		return -ENOTTY;
	if (_IOC_NR(cmd) > SCULLMC_IOC_MAXNR)
		return -ENOTTY;

	/*
	 * The type is a bitmask, and VERIFY_WRITE catches R/W transfers.
	 * Note that the type is user-oriented, while verify_area is
	 * kernel-oriented, so the concept of "read" and "write" is reversed.
	 */
	if (_IOC_DIR(cmd) & _IOC_READ)
		err = !access_ok(VERIFY_WRITE, (void __user *) arg, _IOC_SIZE(cmd));
	else if (_IOC_DIR(cmd) & _IOC_WRITE)
		err = !access_ok(VERIFY_READ, (void __user *) arg, _IOC_SIZE(cmd));
	if (err)
		return -EFAULT;

	switch (cmd) {
		case SCULLMC_IOCRESET:
			scullmc_qset	= SCULLMC_QSET;
			scullmc_quantum = SCULLMC_QUANTUM;
			break;

		case SCULLMC_IOCSQUANTUM:
			ret = __get_user(scullmc_quantum, (int __user *) arg);
			break;

		case SCULLMC_IOCTQUANTUM:
			scullmc_quantum = arg;
			break;

		case SCULLMC_IOCGQUANTUM:
			ret = __put_user(scullmc_quantum, (int __user *) arg);
			break;

		case SCULLMC_IOCQQUANTUM:
			return scullmc_quantum;

		case SCULLMC_IOCXQUANTUM:
			tmp	= scullmc_quantum;
			ret	= __get_user(scullmc_quantum, (int __user *) arg);
			if (ret == 0)
				ret = __put_user(tmp, (int __user *) arg);
			break;

		case SCULLMC_IOCHQUANTUM:
			tmp		= scullmc_quantum;
			scullmc_quantum	= arg;
			return tmp;

		case SCULLMC_IOCSQSET:
			ret = __get_user(scullmc_qset, (int __user *) arg);
			break;

		case SCULLMC_IOCTQSET:
			scullmc_qset = arg;
			break;

		case SCULLMC_IOCGQSET:
			ret = __put_user(scullmc_qset, (int __user *) arg);
			break;

		case SCULLMC_IOCQQSET:
			return scullmc_qset;

		case SCULLMC_IOCXQSET:
			tmp	= scullmc_qset;
			ret	= __get_user(scullmc_qset, (int __user *) arg);
			if (ret == 0)
				ret = __put_user(tmp, (int __user *) arg);
			break;

		case SCULLMC_IOCHQSET:
			tmp = scullmc_qset;
			scullmc_qset = arg;
			return tmp;

		default:	/* redundant as cmd was checked against MAXNR */
			return -ENOTTY;
	}

	return ret;
}

/* The extended operations */
loff_t scullmc_llseek(struct file *filp, loff_t off, int whence)
{
	long newpos;
	struct scullmc_dev *dev	= filp->private_data;

	switch (whence) {
		case 0:		/* SEEK_SET */
			newpos = off;
			break;

		case 1:		/* SEEK_CUR */
			newpos	= filp->f_pos + off;
			break;

		case 2:		/* SEEK_END */
			newpos	= dev->size + off;
			break;

		default:
			return -EINVAL;
	}

	if (newpos < 0)
		return -EINVAL;
	filp->f_pos	= newpos;
	return newpos;
}

/* A simple asynchronous I/O implementation. */

struct async_work {
	struct kiocb *iocb;
	int result;
	struct work_struct work;
};

/* Complete an asynchronous operation */
static void scullmc_do_deferred_op(struct work_struct *work)
{
	//struct async_work *stuff = (struct async_work *) p;
	struct async_work *p = container_of(work, struct async_work, work);
	aio_complete(p->iocb, p->result, 0);
	kfree(p);
}

static ssize_t scullmc_defer_op(int write, struct kiocb *iocb, char __user *buf,
			    size_t count, loff_t pos)
{
	int result;
	struct async_work *stuff;

	/* Copy now while we can access the buffer */
	if (write)
		result = scullmc_write(iocb->ki_filp, buf, count, &pos);
	else
		result = scullmc_read(iocb->ki_filp, buf, count, &pos);

	/* If this is a synchronous IOCB, we return out status now. */
	if (is_sync_kiocb(iocb))
		return result;

	/* Otherwise defer the completion for a few milliseconds. */
	stuff = kmalloc(sizeof(*stuff), GFP_KERNEL);
	if (stuff == NULL)
		return result;	/* No memory, just complete now */
	stuff->iocb	= iocb;
	stuff->result	= result;
	INIT_WORK(&stuff->work, scullmc_do_deferred_op);
	schedule_delayed_work(&stuff->work, HZ/100);
	return -EIOCBQUEUED;
}

static ssize_t scullmc_aio_read(struct kiocb *iocb, const struct iovec *iov,
				unsigned long nr_segs, loff_t pos)
{
	char __user *buf = NULL;
	size_t count = 0;

	return scullmc_defer_op(0, iocb, buf, count, pos);
	//return 0;
}

static ssize_t scullmc_aio_write(struct kiocb *iocb, const struct iovec *iov,
				 unsigned long nr_segs, loff_t pos)
{
	char __user *buf = NULL;
	size_t count = 0;
	return scullmc_defer_op(1, iocb, (char __user *) buf, count, pos);
	//return 0;
}

/* The fops */
struct file_operations scullmc_fops = {
	.owner		= THIS_MODULE,
	.llseek		= scullmc_llseek,
	.read		= scullmc_read,
	.write		= scullmc_write,
	.unlocked_ioctl	= scullmc_ioctl,
	.open		= scullmc_open,
	.release	= scullmc_release,
	.aio_read	= scullmc_aio_read,
	.aio_write	= scullmc_aio_write
};

int scullmc_trim(struct scullmc_dev *dev)
{
	int i;
	struct scullmc_dev *next, *dptr;
	int qset	= dev->qset;

	if (dev->vmas)	/* don't trim: there are active mappings */
		return -EBUSY;

	for (dptr = dev; dptr; dptr = next) {
		if (dptr->data) {
			for (i = 0; i < qset; i++)
				if (dptr->data[i])
					kmem_cache_free(scullmc_cache, dptr->data[i]);

			kfree(dptr->data);
			dptr->data = NULL;
		}

		next = dptr->next;
		if (dptr != dev)
			kfree(dptr);		/* all of them but the first */
	}

	dev->size	= 0;
	dev->qset	= scullmc_qset;
	dev->quantum	= scullmc_qset;
	dev->next	= NULL;
	return 0;
}

static void scullmc_setup_cdev(struct scullmc_dev *dev, int index)
{
	int err;
	int devno = MKDEV(scullmc_major, index);

	cdev_init(&dev->cdev, &scullmc_fops);
	dev->cdev.owner	= THIS_MODULE;
	dev->cdev.ops	= &scullmc_fops;
	err = cdev_add(&dev->cdev, devno, 1);
	if (err)
		printk(KERN_NOTICE "Error %d adding scull%d", err, index);
}

/* And.... the module stuff */

int scullmc_init(void)
{
	int result, i;
	dev_t dev = MKDEV(scullmc_major, 0);

	/* Register major and accept dynamic number */
	if (scullmc_major)
		result = register_chrdev_region(dev, scullmc_devs, "scullmc");
	else {
		result = alloc_chrdev_region(&dev, 0, scullmc_devs, "scullmc");
		scullmc_major = MAJOR(dev);
	}

	if (result < 0)
		return result;

	/*
	 * allocate the devices -- we can't have them static, as the number can
	 * be specified at load time
	 */
	scullmc_devices = kmalloc((scullmc_devs * sizeof(struct scullmc_dev)), GFP_KERNEL);
	if (!scullmc_devices) {
		result = -ENOMEM;
		goto fail_malloc;
	}

	memset (scullmc_devices, 0, scullmc_devs * sizeof (struct scullmc_dev));
	for (i = 0; i < scullmc_devs; i++) {
		scullmc_devices[i].quantum = scullmc_quantum;
		scullmc_devices[i].qset = scullmc_qset;
		sema_init(&scullmc_devices[i].sem, 1);
		scullmc_setup_cdev(scullmc_devices + i, i);
	}

	/*
	scullmc_cache = kmem_cache_create("scullmc", scullmc_quantum, 0,
					 SLAB_HWCACHE_ALIGN, NULL); // no ctor/dtor
	*/
	scullmc_cache = kmem_cache_create("scullmc", sizeof(struct scullmc_dev), 0,
					 SLAB_HWCACHE_ALIGN, NULL); /* no ctor/dtor */
	if (!scullmc_cache) {
		scullmc_cleanup();
		return -ENOMEM;
	}

#ifdef SCULLMC_USE_PROC
	/* create /proc entry here */
#endif
	return 0;

fail_malloc:
	unregister_chrdev_region(dev, scullmc_devs);
	return result;
}

void scullmc_cleanup(void)
{
	int i;
#ifdef SCULLMC_USE_PROC
	/* remove proc entry */
#endif
	for (i = 0; i < scullmc_devs; i++) {
		cdev_del(&scullmc_devices[i].cdev);
		scullmc_trim(scullmc_devices + i);
	}
	kfree(scullmc_devices);

	if (scullmc_cache)
		kmem_cache_destroy(scullmc_cache);
	unregister_chrdev_region(MKDEV (scullmc_major, 0), scullmc_devs);
}

module_init(scullmc_init);
module_exit(scullmc_cleanup);
