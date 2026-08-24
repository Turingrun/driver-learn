#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>

#define DEVICE_NAME "mychar"
#define MAJOR_NUM 200

static int my_open(struct inode *inode, struct file *file) {
  printk(KERN_INFO "mychar: open\n");
  return 0;
}

static int my_release(struct inode *inode, struct file *file) {
  printk(KERN_INFO "mychar: release\n");
  return 0;
}

static ssize_t my_read(struct file *file, char __user *buf, size_t count,
                       loff_t *ppos) {
  printk(KERN_INFO "mychar: read count=%zu\n", count);

  return 0;
}

static ssize_t my_write(struct file *file, const char __user *buf, size_t count,
                        loff_t *ppos) {
  printk(KERN_INFO "mychar: write count=%zu\n", count);

  return count;
}

static const struct file_operations my_fops = {
    .owner = THIS_MODULE,
    .open = my_open,
    .read = my_read,
    .write = my_write,
    .release = my_release,
};

static int __init mychar_init(void) {
  int ret;

  ret = register_chrdev(MAJOR_NUM, DEVICE_NAME, &my_fops);

  if (ret < 0) {
    printk(KERN_ERR "mychar: register_chrdev failed\n");
    return ret;
  }

  printk(KERN_INFO "mychar: init\n");

  return 0;
}

static void __exit mychar_exit(void) {
  unregister_chrdev(MAJOR_NUM, DEVICE_NAME);

  printk(KERN_INFO "mychar: exit\n");
}

module_init(mychar_init);
module_exit(mychar_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("student");
MODULE_DESCRIPTION("Simple character device driver");