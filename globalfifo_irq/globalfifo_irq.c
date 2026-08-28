// myirq.c

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define DEVICE_NAME "myirq"
#define CLASS_NAME "myirq_class"

struct myirq_dev {
  dev_t devno;
  struct cdev cdev;
  struct class *class;
  struct device *device;

  int irq;

  wait_queue_head_t waitq;
  spinlock_t lock;

  unsigned int irq_count;
  unsigned int read_count;
};

static struct myirq_dev *g_dev;

/*
 * 通过 insmod myirq.ko irq=xxx 指定 IRQ 号。
 *
 * 注意：
 * 必须是真实存在、且允许当前驱动注册的 IRQ。
 */
static int irq = -1;
module_param(irq, int, 0444);
MODULE_PARM_DESC(irq, "IRQ number");

/*
 * 中断处理函数
 *
 * 运行上下文：
 *     hard IRQ context
 *
 * 注意：
 *     不能睡眠
 *     不能使用 mutex
 *     不能调用可能睡眠的接口
 */
static irqreturn_t myirq_handler(int irq, void *dev_id) {
  struct myirq_dev *dev = dev_id;
  unsigned long flags;

  /*
   * irq_count 同时可能被：
   *
   * 1. 中断上下文修改
   * 2. read() 进程上下文读取
   *
   * 所以需要并发保护。
   */
  spin_lock_irqsave(&dev->lock, flags);

  dev->irq_count++;

  spin_unlock_irqrestore(&dev->lock, flags);

  /*
   * 唤醒阻塞在 read() 中的进程。
   *
   * wake_up_interruptible() 本身不会让 ISR 睡眠。
   */
  wake_up_interruptible(&dev->waitq);

  return IRQ_HANDLED;
}

static int myirq_open(struct inode *inode, struct file *file) {
  struct myirq_dev *dev;

  /*
   * inode->i_cdev 指向当前字符设备对应的 cdev。
   *
   * container_of() 找到包含这个 cdev 的 myirq_dev。
   */
  dev = container_of(inode->i_cdev, struct myirq_dev, cdev);

  /*
   * 保存到 private_data。
   *
   * 后面的 read() 就可以直接：
   *
   * struct myirq_dev *dev = file->private_data;
   */
  file->private_data = dev;

  return 0;
}

static int myirq_release(struct inode *inode, struct file *file) { return 0; }

static ssize_t myirq_read(struct file *file, char __user *buf, size_t count,
                          loff_t *ppos) {
  struct myirq_dev *dev = file->private_data;
  unsigned long flags;
  unsigned int value;
  int ret;

  if (count < sizeof(value))
    return -EINVAL;

  /*
   * 非阻塞模式：
   *
   * open("/dev/myirq", O_RDONLY | O_NONBLOCK)
   *
   * 如果当前没有新 IRQ，则直接返回 -EAGAIN。
   */
  if (file->f_flags & O_NONBLOCK) {
    if (READ_ONCE(dev->irq_count) == READ_ONCE(dev->read_count))
      return -EAGAIN;
  } else {
    /*
     * 阻塞模式：
     *
     * 如果 irq_count == read_count，
     * 说明没有尚未读取的新中断。
     *
     * 当前进程进入 TASK_INTERRUPTIBLE，
     * 然后让出 CPU。
     *
     * ISR 调用 wake_up_interruptible()
     * 后，该进程会重新成为 runnable。
     */
    ret = wait_event_interruptible(dev->waitq, READ_ONCE(dev->irq_count) !=
                                                   READ_ONCE(dev->read_count));

    if (ret)
      return ret;
  }

  /*
   * 读取 IRQ 计数。
   *
   * 这里必须防止 ISR 同时修改 irq_count。
   */
  spin_lock_irqsave(&dev->lock, flags);

  value = dev->irq_count;

  /*
   * 表示用户已经消费到当前这一批 IRQ。
   */
  dev->read_count = dev->irq_count;

  spin_unlock_irqrestore(&dev->lock, flags);

  /*
   * copy_to_user() 不应该放在 spinlock 临界区里。
   *
   * 因为 copy_to_user() 可能因为缺页等原因
   * 导致睡眠。
   */
  if (copy_to_user(buf, &value, sizeof(value)))
    return -EFAULT;

  return sizeof(value);
}

static const struct file_operations myirq_fops = {
    .owner = THIS_MODULE,
    .open = myirq_open,
    .release = myirq_release,
    .read = myirq_read,
};

static int __init myirq_init(void) {
  int ret;

  if (irq < 0) {
    pr_err("myirq: please specify irq=N\n");
    return -EINVAL;
  }

  g_dev = kzalloc(sizeof(*g_dev), GFP_KERNEL);
  if (!g_dev)
    return -ENOMEM;

  /*
   * 1. 初始化等待队列
   */
  init_waitqueue_head(&g_dev->waitq);

  /*
   * 2. 初始化自旋锁
   */
  spin_lock_init(&g_dev->lock);

  g_dev->irq = irq;

  /*
   * 3. 动态申请字符设备号
   */
  ret = alloc_chrdev_region(&g_dev->devno, 0, 1, DEVICE_NAME);
  if (ret) {
    pr_err("myirq: alloc_chrdev_region failed\n");
    goto err_free_dev;
  }

  /*
   * 4. 初始化并注册 cdev
   */
  cdev_init(&g_dev->cdev, &myirq_fops);
  g_dev->cdev.owner = THIS_MODULE;

  ret = cdev_add(&g_dev->cdev, g_dev->devno, 1);
  if (ret) {
    pr_err("myirq: cdev_add failed\n");
    goto err_unregister_chrdev;
  }

  /*
   * 5. 创建 class
   */
  g_dev->class = class_create(THIS_MODULE, CLASS_NAME);
  if (IS_ERR(g_dev->class)) {
    ret = PTR_ERR(g_dev->class);
    pr_err("myirq: class_create failed\n");
    goto err_cdev_del;
  }

  /*
   * 6. 创建 /dev/myirq
   */
  g_dev->device =
      device_create(g_dev->class, NULL, g_dev->devno, NULL, DEVICE_NAME);

  if (IS_ERR(g_dev->device)) {
    ret = PTR_ERR(g_dev->device);
    pr_err("myirq: device_create failed\n");
    goto err_class_destroy;
  }

  /*
   * 7. 注册 IRQ handler
   *
   * 参数：
   *
   * irq
   *     Linux IRQ number
   *
   * myirq_handler
   *     中断处理函数
   *
   * 0
   *     IRQ flags
   *
   * DEVICE_NAME
   *     /proc/interrupts 中显示的名称
   *
   * g_dev
   *     dev_id，会原样传给 handler
   */
  ret = request_irq(g_dev->irq, myirq_handler, 0, DEVICE_NAME, g_dev);

  if (ret) {
    pr_err("myirq: request_irq(%d) failed: %d\n", g_dev->irq, ret);
    goto err_device_destroy;
  }

  pr_info("myirq: loaded\n");
  pr_info("myirq: irq = %d\n", g_dev->irq);
  pr_info("myirq: major = %d minor = %d\n", MAJOR(g_dev->devno),
          MINOR(g_dev->devno));

  return 0;

err_device_destroy:
  device_destroy(g_dev->class, g_dev->devno);

err_class_destroy:
  class_destroy(g_dev->class);

err_cdev_del:
  cdev_del(&g_dev->cdev);

err_unregister_chrdev:
  unregister_chrdev_region(g_dev->devno, 1);

err_free_dev:
  kfree(g_dev);

  return ret;
}

static void __exit myirq_exit(void) {
  /*
   * 一定要先释放 IRQ。
   *
   * 第二个参数必须和 request_irq()
   * 时传入的 dev_id 对应。
   */
  free_irq(g_dev->irq, g_dev);

  device_destroy(g_dev->class, g_dev->devno);
  class_destroy(g_dev->class);

  cdev_del(&g_dev->cdev);

  unregister_chrdev_region(g_dev->devno, 1);

  kfree(g_dev);

  pr_info("myirq: unloaded\n");
}

module_init(myirq_init);
module_exit(myirq_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Example");
MODULE_DESCRIPTION("Linux IRQ + wait queue example");