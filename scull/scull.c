#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/kernel.h>	/* printk() */
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fs.h>		/* everything... */
#include <linux/errno.h>	/* error codes */
#include <linux/types.h>	/* size_t */
#include <linux/proc_fs.h>
#include <linux/fcntl.h>	/* O_ACCMODE */
#include <linux/seq_file.h>
#include <linux/cdev.h>

#include <linux/uaccess.h>	/* copy_*_user */

#include "scull.h"

MODULE_AUTHOR("Tan Yu Peng");
MODULE_LICENSE("GPL");

int scull_major =   SCULL_MAJOR;
int scull_minor =   0;
int scull_nr_devs = SCULL_NR_DEVS;	/* number of bare scull devices */
int scull_quantum = SCULL_QUANTUM;
int scull_qset =    SCULL_QSET;

module_param(scull_major, int, S_IRUGO);
module_param(scull_minor, int, S_IRUGO);
module_param(scull_nr_devs, int, S_IRUGO);
module_param(scull_quantum, int, S_IRUGO);
module_param(scull_qset, int, S_IRUGO);

struct scull_dev *scull_devices;	/* allocated in scull_init_module */

/* ---------------------- helper functions ---------------------- */

int scull_trim(struct scull_dev *dev)
{
    struct scull_qset *next, *dptr;
    int qset = dev->qset;                       /* "dev" is not-null */

    for (dptr = dev->data; dptr; dptr = next) { /* all the list items */
        if (dptr->data) {
            for (int i = 0; i < qset; i++) kfree(dptr->data[i]);
            kfree(dptr->data);
            dptr->data = NULL;
        }
        next = dptr->next;
        kfree(dptr);
    }

    dev->size = 0;
    dev->quantum = SCULL_QUANTUM;
    dev->qset = SCULL_QSET;
    dev->data = NULL;

    return 0;
}

/*
 * Follow the list
 */
struct scull_qset *scull_follow(struct scull_dev *dev, int n)
{
    struct scull_qset *qs = dev->data;

    /* Allocate first qset explicitly if need be */
    if (! qs) {
        qs = dev->data = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
        if (qs == NULL)
            return NULL;  /* Never mind */
        memset(qs, 0, sizeof(struct scull_qset));
    }

    /* Then follow the list */
    while (n--) {
        if (!qs->next) {
            qs->next = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
            if (qs->next == NULL)
                return NULL;  /* Never mind */
            memset(qs->next, 0, sizeof(struct scull_qset));
        }
        qs = qs->next;
        continue;
    }
    return qs;
}

/* ---------------------- file operations ---------------------- */

int scull_open(struct inode *inode, struct file *filp)
{
    struct scull_dev *dev;    /* device information */

    dev = container_of(inode->i_cdev, struct scull_dev, cdev);
    filp->private_data = dev; /* for other methods */

    /* now trim to 0 the length of the device if open was write-only */
    if ( (filp->f_flags & O_ACCMODE) == O_WRONLY ) {
        scull_trim(dev);      /* ignore errors */
    }
    return 0;                 /* success */
}

ssize_t scull_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    struct scull_dev *dev = filp->private_data;
    struct scull_qset *dptr;
    int quantum = dev->quantum, qset = dev->qset;
    int itemsize = quantum * qset;     /* bytes in a quantum set (linked-list node) */
    int item, s_pos, q_pos, rest;
    ssize_t retval = 0;

    if (down_interruptible(&dev->sem)) /* try to acquire semaphore */
        return -ERESTARTSYS;

    if (*f_pos >= dev->size)           /* current read position > device size */
        goto out;

    if (*f_pos + count > dev->size)    /* only read till device size */
        count = dev->size - *f_pos;

    item = (long)*f_pos / itemsize;    /* which node in linked-list? */
    rest = (long)*f_pos % itemsize;    /* which data in this node has been read? */
    s_pos = rest / quantum;            /* index of quantum (array element) in quantum set (array) */
    q_pos = rest % quantum;            /* offset into quantum (chunk of data) */

    dptr = scull_follow(dev, item);    /* get linked-list node (defined elsewhere) */

    /* don't account for holes, return if data is invalid */
    if (dptr == NULL || !dptr->data || !dptr->data[s_pos])
        goto out;

    if (count > quantum - q_pos)       /* read only to the end of this quantum */
        count = quantum - q_pos;

    if (copy_to_user(buf, dptr->data[s_pos] + q_pos, count)) {
        retval = -EFAULT;
        goto out;
    }

    *f_pos += count;
    retval = count;

out:
    up(&dev->sem);
    return retval;
}

ssize_t scull_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    struct scull_dev *dev = filp->private_data;
    struct scull_qset *dptr;
    int quantum = dev->quantum, qset = dev->qset;
    int itemsize = quantum *qset;
    int item, s_pos, q_pos, rest;
    ssize_t retval = -ENOMEM;          /* value used in "goto out" statements */

    if (down_interruptible(&dev->sem)) return -ERESTARTSYS;

    /* find linked-list item, quantum set index, quantum offset */
    item = (long)*f_pos / itemsize;
    rest = (long)*f_pos % itemsize;
    s_pos = rest / quantum;
    q_pos = rest % quantum;

    dptr = scull_follow(dev, item);    /* follow the list up to the right position */

    if (dptr == NULL) goto out;        /* end of linked-list */

    if (!dptr->data) {                 /* allocate array of pointers */
        dptr->data = kmalloc(qset * sizeof(char *), GFP_KERNEL);
        if (!dptr->data) goto out;
        memset(dptr->data, 0, qset * sizeof(char *));
    }

    if (!dptr->data[s_pos]) {          /* allocate pointer data (quantum) */
        dptr->data[s_pos] = kmalloc(quantum, GFP_KERNEL);
        if (!dptr->data[s_pos]) goto out;
    }

    /* write only up to the end of this quantum */
    if (count > quantum - q_pos) count = quantum - q_pos;

    if (copy_from_user(dptr->data[s_pos]+q_pos, buf, count)) {
        retval = -EFAULT;
        goto out;
    }

    *f_pos += count;
    retval = count;

    /* update the size */
    if (dev->size < *f_pos)
        dev->size = *f_pos;

out:
    up(&dev->sem);
    return retval;

}

int scull_release(struct inode *inode, struct file *filp)
{
    return 0;
}

/* ---------------------- module stuff ---------------------- */

struct file_operations scull_fops = {
    .owner = THIS_MODULE,
    .open = scull_open,
    .read = scull_read,
    .write = scull_write,
    .release = scull_release,
};

/*
 * The cleanup function is used to handle initialization failures as well.
 * Thefore, it must be careful to work correctly even if some of the items
 * have not been initialized
 */
void scull_cleanup_module(void)
{
    dev_t devno = MKDEV(scull_major, scull_minor);

    /* Get rid of our char dev entries */
    if (scull_devices) {
        for (int i = 0; i < scull_nr_devs; i++) {
            scull_trim(scull_devices + i);
            cdev_del(&scull_devices[i].cdev);
        }
        kfree(scull_devices);
    }

#ifdef SCULL_DEBUG /* use proc only if debugging */
    scull_remove_proc();
#endif

    /* cleanup_module is never called if registering failed */
    unregister_chrdev_region(devno, scull_nr_devs);
}

/*
 * Set up the char_dev structure for this device.
 */
static void scull_setup_cdev(struct scull_dev *dev, int index)
{
    int err, devno = MKDEV(scull_major, scull_minor + index);

    cdev_init(&dev->cdev, &scull_fops);
    dev->cdev.owner = THIS_MODULE;
    err = cdev_add (&dev->cdev, devno, 1);
    /* Fail gracefully if need be */
    if (err)
        printk(KERN_NOTICE "Error %d adding scull%d", err, index);
}


int scull_init_module(void)
{
    int result;
    dev_t dev = 0;

    /*
     * Get a range of minor numbers to work with, asking for a dynamic
     * major unless directed otherwise at load time.
     */
    if (scull_major) {
        dev = MKDEV(scull_major, scull_minor);
        result = register_chrdev_region(dev, scull_nr_devs, "scull");
    } else {
        result = alloc_chrdev_region(&dev, scull_minor, scull_nr_devs,
                "scull");
        scull_major = MAJOR(dev);
    }
    if (result < 0) {
        printk(KERN_WARNING "scull: can't get major %d\n", scull_major);
        return result;
    }

    /*
     * allocate the devices -- we can't have them static, as the number
     * can be specified at load time
     */
    scull_devices = kmalloc(scull_nr_devs * sizeof(struct scull_dev), GFP_KERNEL);
    if (!scull_devices) {
        result = -ENOMEM;
        goto fail;  /* Make this more graceful */
    }
    memset(scull_devices, 0, scull_nr_devs * sizeof(struct scull_dev));

    /* Initialize each device. */
    for (int i = 0; i < scull_nr_devs; i++) {
        scull_devices[i].quantum = scull_quantum;
        scull_devices[i].qset = scull_qset;
        sema_init(&scull_devices[i].sem, 1);
        scull_setup_cdev(&scull_devices[i], i);
    }

    /* At this point call the init function for any friend device */
    dev = MKDEV(scull_major, scull_minor + scull_nr_devs);

#ifdef SCULL_DEBUG /* only when debugging */
    scull_create_proc();
#endif

    return 0; /* succeed */

fail:
    scull_cleanup_module();
    return result;
}

module_init(scull_init_module);
module_exit(scull_cleanup_module);
