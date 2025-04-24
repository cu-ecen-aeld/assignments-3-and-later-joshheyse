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
#include "aesd_ioctl.h"
// clang-format on
int aesd_major = 0; // use dynamic major
int aesd_minor = 0;

MODULE_AUTHOR("Josh Heyse");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp) {
  struct aesd_dev *dev;
  PDEBUG("open");

  dev = container_of(inode->i_cdev, struct aesd_dev, cdev); // get the device structure

  filp->private_data = dev; // for other methods
  return 0;
}

int aesd_release(struct inode *inode, struct file *filp) {
  struct aesd_dev *dev = filp->private_data;

  PDEBUG("release");
  if (dev == NULL) {
    PDEBUG("release: no private data");
    return 0; // Should this be -EINVAL?
  }
  return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos) {
  ssize_t retval = 0;
  struct aesd_dev *dev = filp->private_data;
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

  ssize_t bytes_to_copy = min(entry->size - entry_offset_byte_rtn, count);
  if (bytes_to_copy == 0) {
    retval = 0;
    goto out;
  }

  PDEBUG("read: copying %zu bytes", bytes_to_copy);
  retval = copy_to_user(buf, entry->buffptr + entry_offset_byte_rtn, bytes_to_copy);
  if (retval) {
    PDEBUG("read: copy_to_user failed");
    retval = -EFAULT;
    goto out;
  }
  *f_pos += bytes_to_copy;
  retval = bytes_to_copy;

out:
  mutex_unlock(&dev->lock);
  PDEBUG("read: returning %zd", retval);
  return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos) {
  PDEBUG("write %zu bytes with offset %lld", count, *f_pos);
  ssize_t retval = -ENOMEM;
  struct aesd_dev *dev = filp->private_data;
  // Store a pointer to the entire allocated buffer in buffer and use working as a span (pointer + size)
  struct aesd_buffer_entry working;
  ssize_t combined_count = dev->partial.size + count;
  PDEBUG("write: combined count %zu", combined_count);
  void *buffer = kmalloc(combined_count, GFP_KERNEL);
  void *new_line_ptr = NULL;
  working.buffptr = buffer;
  working.size = combined_count;

  if (working.buffptr == NULL) {
    PDEBUG("write: kmalloc failed");
    goto out;
  }

  if (dev->partial.buffptr != NULL) {
    memcpy((void *)working.buffptr, dev->partial.buffptr, dev->partial.size);
  }

  if (copy_from_user((void *)working.buffptr + dev->partial.size, buf, count)) {
    PDEBUG("write: copy_from_user failed");
    goto out;
    retval = -EFAULT;
  }

  if (dev->partial.buffptr != NULL) {
    kfree_const(dev->partial.buffptr);
    dev->partial.buffptr = NULL;
    dev->partial.size = 0;
  }

  if (mutex_lock_interruptible(&dev->lock)) {
    PDEBUG("write: lock failed");
    retval = -ERESTARTSYS;
    goto out;
  }

  new_line_ptr = memchr(working.buffptr, '\n', working.size);
  while (working.size > 0 && new_line_ptr != NULL) {
    size_t bytes_to_copy = new_line_ptr - (void *)working.buffptr + 1;
    struct aesd_buffer_entry *entry = kmalloc(sizeof(struct aesd_buffer_entry), GFP_KERNEL);
    entry->buffptr = kmemdup(working.buffptr, bytes_to_copy, GFP_KERNEL);
    if (entry->buffptr == NULL) {
      PDEBUG("write: kmemdup failed");
      retval = -EINVAL;
      goto release;
    }
    entry->size = bytes_to_copy;
    PDEBUG("write: adding entry %zu bytes", bytes_to_copy);
    const char *garbage = aesd_circular_buffer_add_entry(&dev->circular_buffer, entry);
    if (garbage != NULL) {
      kfree_const(garbage);
    }
    working.size -= bytes_to_copy;
    working.buffptr += bytes_to_copy;
    new_line_ptr = memchr(working.buffptr, '\n', working.size);
  }

  if (working.size > 0) {
    PDEBUG("write: partial write %zu bytes", working.size);
    dev->partial.buffptr = kmemdup(working.buffptr, working.size, GFP_KERNEL);
    if (dev->partial.buffptr == NULL) {
      retval = -ENOMEM;
      goto release;
    }
    dev->partial.size = working.size;
  }
  *f_pos += count;
  retval = count;

release:
  mutex_unlock(&dev->lock);
  PDEBUG("write: release lock");
out:
  if (buffer != NULL) {
    kfree_const(buffer);
  }
  PDEBUG("write: returning %zd", retval);
  return retval;
}

static loff_t aesd_llseek(struct file *filp, loff_t offset, int whence) {
  struct aesd_dev *dev = filp->private_data;
  loff_t new_pos;

  PDEBUG("llseek %lld %d", offset, whence);
  if (mutex_lock_interruptible(&dev->lock)) {
    return -ERESTARTSYS;
  }

  loff_t totalSize = 0;
  for (int i = 0; i < AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED; i++) {
    totalSize += dev->circular_buffer.entry[i].size;
  }

  new_pos = fixed_size_llseek(filp, offset, whence, totalSize);
  PDEBUG("llseek %lld %d -> %lld", offset, whence, new_pos);
  mutex_unlock(&dev->lock);
  return new_pos;
}

static long aesd_adjust_file_offset(struct file *filp, struct aesd_seekto *seekto) {
  struct aesd_dev *dev = filp->private_data;
  loff_t retval = -EINVAL;
  int i;

  if (seekto->write_cmd >= AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED) {
    return -EINVAL;
  }

  if (mutex_lock_interruptible(&dev->lock)) {
    return -ERESTARTSYS;
  }

  for (i = 0; i < seekto->write_cmd; i++) {
    retval += dev->circular_buffer.entry[i].size;
  }
  if (seekto->write_cmd_offset > dev->circular_buffer.entry[i].size) {
    retval = -EINVAL;
    goto out;
  }
  retval += seekto->write_cmd_offset;

  filp->f_pos = retval;

out:
  mutex_unlock(&dev->lock);
  return retval;
}

static long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
  PDEBUG("ioctl");
  if (_IOC_TYPE(cmd) != AESD_IOC_MAGIC) {
    return -ENOTTY;
  }
  if (_IOC_NR(cmd) > AESDCHAR_IOC_MAXNR) {
    return -ENOTTY;
  }
  switch (cmd) {
  case AESDCHAR_IOCSEEKTO: {
    struct aesd_seekto seekto;
    if (copy_from_user(&seekto, (const void __user *)arg, sizeof(struct aesd_seekto))) {
      return -EFAULT;
    }
    return aesd_adjust_file_offset(filp, &seekto);
  }
  default:
    return -ENOTTY;
  }
}

struct file_operations aesd_fops = {.owner = THIS_MODULE,
                                    .read = aesd_read,
                                    .write = aesd_write,
                                    .open = aesd_open,
                                    .release = aesd_release,
                                    .llseek = aesd_llseek,
                                    .compat_ioctl = aesd_ioctl};

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
  PDEBUG("\n\n\naesd_init_module: major %d\n", aesd_major);
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

  PDEBUG("aesd_cleanup_module\n\n\n");
  cdev_del(&aesd_device.cdev);

  AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.circular_buffer, index) {
    if (entry->buffptr != NULL) {
      kfree_const(entry->buffptr);
      entry->buffptr = NULL;
    }
  }
  mutex_destroy(&aesd_device.lock);
  if (aesd_device.partial.buffptr != NULL) {
    kfree_const(aesd_device.partial.buffptr);
    aesd_device.partial.buffptr = NULL;
    aesd_device.partial.size = 0;
  }

  unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
