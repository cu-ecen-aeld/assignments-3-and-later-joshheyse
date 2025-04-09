/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */
// clang-format off
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/types.h>
#include "aesd-circular-buffer.h"
#include "aesdchar.h"
// clang-format on
int aesd_major = 0; // use dynamic major
int aesd_minor = 0;

MODULE_AUTHOR("Josh Heyse");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

// The assignment text and video mention putting the working buffer on the device
// structure, but this is not a good idea.  The buffer should be in the private data
// structure, as it is only used in the context of a single open file.  If multiple writes were occuring
// simultaneously, the buffer would be receive mixed (and possibly corrupted) data.
struct private_data {
  struct aesd_dev *dev;
  struct aesd_buffer_entry partial;
};

int aesd_open(struct inode *inode, struct file *filp) {
  struct aesd_dev *dev;
  struct private_data *priv_data;
  PDEBUG("open");

  dev = container_of(inode->i_cdev, struct aesd_dev, cdev); // get the device structure
  priv_data = kmalloc(sizeof(struct private_data), GFP_KERNEL);
  if (priv_data == NULL) {
    PDEBUG("open: kmalloc failed");
    return -ENOMEM;
  }

  priv_data->dev = dev;
  priv_data->partial.buffptr = NULL;
  priv_data->partial.size = 0;
  filp->private_data = priv_data; // for other methods
  return 0;
}

int aesd_release(struct inode *inode, struct file *filp) {
  struct private_data *priv_data = filp->private_data;

  PDEBUG("release");
  if (priv_data == NULL) {
    PDEBUG("release: no private data");
    return 0; // Should this be -EINVAL?
  }

  if (priv_data->partial.buffptr != NULL) {
    kfree_const(priv_data->partial.buffptr);
    priv_data->partial.buffptr = NULL;
  }
  kfree_const(priv_data);
  return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos) {
  ssize_t retval = 0;
  struct private_data *priv_data = filp->private_data;
  struct aesd_dev *dev = priv_data->dev;
  struct aesd_buffer_entry *entry;
  size_t entry_offset_byte_rtn;
  PDEBUG("read %zu bytes with offset %lld", count, *f_pos);

  if (mutex_lock_interruptible(&dev->lock)) {
    return -ERESTARTSYS;
  }
  entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->circular_buffer, *f_pos, &entry_offset_byte_rtn);
  if (entry == NULL) {
    PDEBUG("read: no entry found");
    retval = 0; // end of file
    goto out;
  }
  retval = copy_to_user(buf, entry->buffptr + entry_offset_byte_rtn, min(entry->size - entry_offset_byte_rtn, count));
  if (retval) {
    PDEBUG("read: copy_to_user failed");
    retval = -EFAULT;
    goto out;
  }

  if (count >= entry->size - entry_offset_byte_rtn) {
    dev->circular_buffer.out_offs = (dev->circular_buffer.out_offs + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    *f_pos += entry->size - entry_offset_byte_rtn;
  } else {
    *f_pos += count;
  }

out:
  mutex_unlock(&dev->lock);
  return retval;
}

int store_remaining_in_working(struct aesd_buffer_entry *partial, struct aesd_buffer_entry working) {
  void *buffptr = kmalloc(partial->size + working.size, GFP_KERNEL);
  if (buffptr == NULL) {
    PDEBUG("store_remaining_in_working: kmalloc failed");
    return -ENOMEM;
  }
  memcpy(buffptr, partial->buffptr, partial->size);
  memcpy(buffptr + partial->size, working.buffptr, working.size);
  kfree_const(partial->buffptr);
  partial->size = partial->size + working.size;
  partial->buffptr = buffptr;
  return 0;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos) {
  ssize_t retval = -ENOMEM;
  struct private_data *priv_data = filp->private_data;
  // Store a pointer to the entire allocated buffer in buffer and use working as a span (pointer + size)
  struct aesd_buffer_entry working;
  void *buffer = kmalloc(count, GFP_KERNEL);
  void *new_line_ptr = NULL;
  PDEBUG("write %zu bytes with offset %lld", count, *f_pos);
  working.buffptr = buffer;
  working.size = count;
  if (working.buffptr == NULL) {
    PDEBUG("write: kmalloc failed");
    goto out;
  }
  if (copy_from_user((void *)working.buffptr, buf, count)) {
    PDEBUG("write: copy_from_user failed");
    goto out;
    retval = -EFAULT;
  }

  new_line_ptr = memchr(working.buffptr, '\n', working.size);
  if (new_line_ptr == NULL) {
    PDEBUG("write: no newline found, copying to private_data working buffer_entry");
    retval = store_remaining_in_working(&priv_data->partial, working);
    if (retval != 0) {
      PDEBUG("write: store_remaining_in_working failed");
      goto out;
    }
    retval = count;
    goto out;
  } else {
    if (mutex_lock_interruptible(&priv_data->dev->lock)) {
      retval = -ERESTARTSYS;
      goto out;
    }

    while (working.size > 0 && new_line_ptr != NULL) {
      size_t bytes_to_copy = new_line_ptr - (void *)working.buffptr + 1;
      if (bytes_to_copy > 0) {
        struct aesd_buffer_entry *entry = kmalloc(sizeof(struct aesd_buffer_entry), GFP_KERNEL);
        entry->buffptr = kmemdup(working.buffptr, bytes_to_copy, GFP_KERNEL);
        if (entry->buffptr == NULL) {
          PDEBUG("write: kmemdup failed");
          retval = -EINVAL;
          goto release;
        }
        entry->size = bytes_to_copy;
        aesd_circular_buffer_add_entry(&priv_data->dev->circular_buffer, entry);
        working.size -= bytes_to_copy;
        working.buffptr += bytes_to_copy;
      }
      new_line_ptr = memchr(working.buffptr, '\n', working.size);
    }
  }

release:
  mutex_unlock(&priv_data->dev->lock);
out:
  if (buffer != NULL) {
    kfree_const(buffer);
  }
  return retval;
}
struct file_operations aesd_fops = {
    .owner = THIS_MODULE,
    .read = aesd_read,
    .write = aesd_write,
    .open = aesd_open,
    .release = aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev) {
  int err, devno = MKDEV(aesd_major, aesd_minor);

  cdev_init(&dev->cdev, &aesd_fops);
  dev->cdev.owner = THIS_MODULE;
  dev->cdev.ops = &aesd_fops;
  err = cdev_add(&dev->cdev, devno, 1);
  if (err) {
    printk(KERN_ERR "Error %d adding aesd cdev", err);
  }
  return err;
}

int aesd_init_module(void) {
  dev_t dev = 0;
  int result;
  result = alloc_chrdev_region(&dev, aesd_minor, 1, "aesdchar");
  aesd_major = MAJOR(dev);
  if (result < 0) {
    printk(KERN_WARNING "Can't get major %d\n", aesd_major);
    return result;
  }
  memset(&aesd_device, 0, sizeof(struct aesd_dev));

  mutex_init(&aesd_device.lock);
  aesd_circular_buffer_init(&aesd_device.circular_buffer);

  result = aesd_setup_cdev(&aesd_device);

  if (result) {
    unregister_chrdev_region(dev, 1);
  }
  return result;
}

void aesd_cleanup_module(void) {
  dev_t devno = MKDEV(aesd_major, aesd_minor);
  int index;
  struct aesd_buffer_entry *entry;

  cdev_del(&aesd_device.cdev);

  AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.circular_buffer, index) {
    if (entry->buffptr != NULL) {
      kfree_const(entry->buffptr);
      entry->buffptr = NULL;
    }
  }
  mutex_destroy(&aesd_device.lock);

  unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
