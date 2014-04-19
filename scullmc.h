/*
 * Definitions for the scullmc char module
 */

#include <linux/ioctl.h>
#include <linux/cdev.h>
#include <linux/semaphore.h>

/* Debugging Macros */
#undef PDEBUG
#ifdef SCULLMC_DEBUG
#    ifdef __KERNEL__
#	define PDEBUG(fmt, args...) printk(KERN_DEBUG "scullmc: " fmt, ## args)
#    else
	/* user-space */
#	define PDEBUG(fmt, args...) fprintf(stderr, fmt, ## args)
#    endif
#else
#    define PDEBUG(fmt, args...)	/* no debugging done */
#endif

#undef PDEBUGG
#define PDEBUG(fmt, args...)

#define SCULLMC_MAJOR	0		/* dynamic major by default */
#define SCULLMC_DEVS	4

/*
 * The bare device is a variable-length region of memory.
 * Use a linked list of indirect blocks.
 *
 * "scullmc_dev->data" points to an array of pointers, each pointer
 * refers to a memory page.
 *
 * The array (quantum-set) is SCULLMC_QSET long.
 */

#define SCULLMC_QUANTUM	4000
#define SCULLMC_QSET	500

struct scullmc_dev {
	void **data;
	struct scullmc_dev *next;	/* next list-item */
	int vmas;		/* active mappings */
	int quantum;		/* the current allocation size */
	int qset;		/* the current array size */
	size_t size;		/* 32-bit will suffice */
	struct semaphore sem;
	struct cdev cdev;
};

extern struct scullmc_dev *scullmc_devices;

extern struct file_operations scullmc_fops;

/* the different configurable parameters */
extern int scullmc_major;	/* main.c */
extern int scullmc_devs;
extern int scullmc_quantum;
extern int scullmc_qset;

/* prototypes for shared functions */
int scullmc_trim(struct scullmc_dev *dev);
struct scullmc_dev *scullmc_follow(struct scullmc_dev *dev, int n);

#ifdef SCULLMC_DEBUG
#    define SCULLMC_USE_PROC
#endif

/*
 * ioctl definitions
 */

/* Use "K" as magic number *TEMPORARILY*/
#define SCULLMC_IOC_MAGIC	'K'
#define SCULLMC_IOCRESET	_IO(SCULLMC_IOC_MAGIC, 0)

/*
 * S means "Set" through a pointer
 * T means "Tell" directly
 * G means "Get" to a pointed-to var)
 * Q means "Query", response is on the return value
 * X means "eXchange": G and S atomically
 * H means "sHift": T and Q atomically
 */
#define SCULLMC_IOCSQUANTUM	_IOW(SCULLMC_IOC_MAGIC,  1, int)
#define SCULLMC_IOCTQUANTUM	_IO(SCULLMC_IOC_MAGIC,   2)
#define SCULLMC_IOCGQUANTUM	_IOR(SCULLMC_IOC_MAGIC,  3, int)
#define SCULLMC_IOCQQUANTUM	_IO(SCULLMC_IOC_MAGIC,   4)
#define SCULLMC_IOCXQUANTUM	_IOWR(SCULLMC_IOC_MAGIC, 5, int)
#define SCULLMC_IOCHQUANTUM	_IO(SCULLMC_IOC_MAGIC,   6)
#define SCULLMC_IOCSQSET	_IOW(SCULLMC_IOC_MAGIC,  7, int)
#define SCULLMC_IOCTQSET	_IO(SCULLMC_IOC_MAGIC,   8)
#define SCULLMC_IOCGQSET	_IOR(SCULLMC_IOC_MAGIC,  9, int)
#define SCULLMC_IOCQQSET	_IO(SCULLMC_IOC_MAGIC,   10)
#define SCULLMC_IOCXQSET	_IOWR(SCULLMC_IOC_MAGIC, 11, int)
#define SCULLMC_IOCHQSET	_IO(SCULLMC_IOC_MAGIC,   12)

#define SCULLMC_IOC_MAXNR	12
