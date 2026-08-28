// myirq.c
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#define DRIVER_NAME "myirq"
#define DEVICE_NAME "myirq"
#define CLASS_NAME "myirq_class"

struct myirq_dev {
  struct device *dev;

  /* Linux IRQ number */
  int irq;

  /* 字符设备 */
  dev_t devno;
  struct cdev cdev;
  struct class *class;
  struct device *char_dev;

  /* 阻塞 read() 使用 */
  wait_queue_head_t waitq;

  /* 保护 irq_count / read_count */
  spinlock_t lock;

  /*
   * irq_count:
   *   IRQ 总共发生了多少次
   *
   * read_count:
   *   用户已经消费到第几次
   */
  unsigned int irq_count;
  unsigned int read_count;
};

/*
 * threaded IRQ handler
 *
 * 注意：
 * 这里不是 hard IRQ context，
 * 而是 IRQ thread context。
 *
 * 因此这里允许睡眠。
 */
static irqreturn_t myirq_thread(int irq, void *dev_id) {
  struct myirq_dev *mydev = dev_id;
  unsigned long flags;
  unsigned int count;

  /*
   * 修改共享数据。
   *
   * read() 也会访问这些变量，
   * 因此使用锁保护。
   */
  spin_lock_irqsave(&mydev->lock, flags);

  mydev->irq_count++;
  count = mydev->irq_count;

  spin_unlock_irqrestore(&mydev->lock, flags);

  /*
   * 唤醒正在等待 IRQ 的 read() 进程。
   */
  wake_up_interruptible(&mydev->waitq);

  dev_info(mydev->dev, "interrupt occurred, count = %u\n", count);

  return IRQ_HANDLED;
}

static int myirq_open(struct inode *inode, struct file *file) {
  struct myirq_dev *mydev;

  /*
   * inode->i_cdev
   *        ↓
   * 找到外层 struct myirq_dev
   */
  mydev = container_of(inode->i_cdev, struct myirq_dev, cdev);

  file->private_data = mydev;

  return 0;
}

static int myirq_release(struct inode *inode, struct file *file) { return 0; }

static ssize_t myirq_read(struct file *file, char __user *buf, size_t count,
                          loff_t *ppos) {
  struct myirq_dev *mydev = file->private_data;

  unsigned long flags;
  unsigned int value;

  int ret;

  if (count < sizeof(value))
    return -EINVAL;

  /*
   * 非阻塞模式：
   *
   * open(..., O_NONBLOCK)
   */
  if (file->f_flags & O_NONBLOCK) {

    if (READ_ONCE(mydev->irq_count) == READ_ONCE(mydev->read_count))
      return -EAGAIN;

  } else {

    /*
     * 阻塞模式。
     *
     * 没有新 IRQ 时：
     *
     * irq_count == read_count
     *
     * 当前进程睡眠。
     */
    ret = wait_event_interruptible(mydev->waitq,
                                   READ_ONCE(mydev->irq_count) !=
                                       READ_ONCE(mydev->read_count));

    if (ret)
      return ret;
  }

  /*
   * IRQ 已经发生。
   *
   * 获取最新 IRQ count，
   * 同时把 read_count 更新到当前位置。
   */
  spin_lock_irqsave(&mydev->lock, flags);

  value = mydev->irq_count;
  mydev->read_count = mydev->irq_count;

  spin_unlock_irqrestore(&mydev->lock, flags);

  /*
   * copy_to_user() 放到 spinlock 外面。
   *
   * 因为它存在睡眠可能。
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

/*
 * platform driver probe
 *
 * Device Tree:
 *
 * compatible = "demo,myirq";
 *
 * 匹配以后会进入这里。
 */
static int myirq_probe(struct platform_device *pdev) {
  struct myirq_dev *mydev;
  int ret;

  dev_info(&pdev->dev, "probe start\n");

  /*
   * 1.
   * 创建设备私有数据。
   *
   * devm_kzalloc()：
   *
   * platform device 被移除以后，
   * Linux 自动释放这块内存。
   */
  mydev = devm_kzalloc(&pdev->dev, sizeof(*mydev), GFP_KERNEL);

  if (!mydev)
    return -ENOMEM;

  mydev->dev = &pdev->dev;

  /*
   * 2.
   * 初始化 wait queue
   */
  init_waitqueue_head(&mydev->waitq);

  /*
   * 3.
   * 初始化 spinlock
   */
  spin_lock_init(&mydev->lock);

  /*
   * 4.
   * 从 Device Tree 获取 IRQ。
   *
   * 对应：
   *
   * interrupt-parent = <&gpio3>;
   *
   * interrupts =
   *     <RK_PB4 IRQ_TYPE_EDGE_FALLING>;
   *
   *
   * 这里返回的是 Linux IRQ number，
   * 不是 GPIO number。
   */
  mydev->irq = platform_get_irq(pdev, 0);

  if (mydev->irq < 0) {
    dev_err(&pdev->dev, "platform_get_irq failed: %d\n", mydev->irq);

    return mydev->irq;
  }

  dev_info(&pdev->dev, "Linux IRQ number = %d\n", mydev->irq);

  /*
   * 5.
   * 注册 threaded IRQ
   *
   * 第二个 handler = NULL
   *
   * 表示我们不提供自己的
   * primary hard IRQ handler。
   *
   *
   * myirq_thread:
   *
   * 实际的 threaded IRQ handler。
   *
   *
   * IRQF_ONESHOT：
   *
   * 在线程处理完成以前，
   * 保持 IRQ line 处于适当的 mask 状态。
   *
   *
   * 注意：
   *
   * 不需要再写：
   *
   * IRQF_TRIGGER_FALLING
   *
   * 因为触发方式已经在 Device Tree：
   *
   * IRQ_TYPE_EDGE_FALLING
   *
   * 中描述了。
   */
  ret = devm_request_threaded_irq(&pdev->dev, mydev->irq, NULL, myirq_thread,
                                  IRQF_ONESHOT, DRIVER_NAME, mydev);

  if (ret) {
    dev_err(&pdev->dev, "request IRQ %d failed: %d\n", mydev->irq, ret);

    return ret;
  }

  /*
   * 6.
   * 动态申请字符设备号。
   */
  ret = alloc_chrdev_region(&mydev->devno, 0, 1, DEVICE_NAME);

  if (ret) {
    dev_err(&pdev->dev, "alloc_chrdev_region failed: %d\n", ret);

    return ret;
  }

  /*
   * 7.
   * 初始化 cdev
   */
  cdev_init(&mydev->cdev, &myirq_fops);

  mydev->cdev.owner = THIS_MODULE;

  /*
   * 8.
   * 注册 cdev
   */
  ret = cdev_add(&mydev->cdev, mydev->devno, 1);

  if (ret) {
    dev_err(&pdev->dev, "cdev_add failed: %d\n", ret);

    goto err_unregister_chrdev;
  }

  /*
   * 9.
   * 创建 class
   */
  mydev->class = class_create(THIS_MODULE, CLASS_NAME);

  if (IS_ERR(mydev->class)) {
    ret = PTR_ERR(mydev->class);

    dev_err(&pdev->dev, "class_create failed: %d\n", ret);

    goto err_cdev_del;
  }

  /*
   * 10.
   * 创建：
   *
   * /dev/myirq
   */
  mydev->char_dev =
      device_create(mydev->class, NULL, mydev->devno, NULL, DEVICE_NAME);

  if (IS_ERR(mydev->char_dev)) {
    ret = PTR_ERR(mydev->char_dev);

    dev_err(&pdev->dev, "device_create failed: %d\n", ret);

    goto err_class_destroy;
  }

  /*
   * 保存设备私有数据。
   *
   * remove() 时可以通过：
   *
   * platform_get_drvdata()
   *
   * 再拿回来。
   */
  platform_set_drvdata(pdev, mydev);

  dev_info(&pdev->dev, "myirq probe success\n");

  dev_info(&pdev->dev, "device node: /dev/%s\n", DEVICE_NAME);

  return 0;

err_class_destroy:

  class_destroy(mydev->class);

err_cdev_del:

  cdev_del(&mydev->cdev);

err_unregister_chrdev:

  unregister_chrdev_region(mydev->devno, 1);

  return ret;
}

/*
 * platform device 被移除或者模块卸载时调用。
 */
static int myirq_remove(struct platform_device *pdev) {
  struct myirq_dev *mydev;

  mydev = platform_get_drvdata(pdev);

  /*
   * devm_request_threaded_irq()
   * 不需要手动 free_irq()。
   *
   * platform device 生命周期结束时，
   * devres 会自动释放 IRQ。
   */

  device_destroy(mydev->class, mydev->devno);

  class_destroy(mydev->class);

  cdev_del(&mydev->cdev);

  unregister_chrdev_region(mydev->devno, 1);

  dev_info(&pdev->dev, "myirq removed\n");

  return 0;
}

/*
 * Device Tree match table
 */
static const struct of_device_id myirq_of_match[] = {

    {
        .compatible = "demo,myirq",
    },

    {}};

MODULE_DEVICE_TABLE(of, myirq_of_match);

/*
 * platform driver
 */
static struct platform_driver myirq_driver = {

    .probe = myirq_probe,
    .remove = myirq_remove,

    .driver =
        {
            .name = DRIVER_NAME,
            .of_match_table = myirq_of_match,
        },
};

/*
 * 等价于自动完成：
 *
 * module_init()
 * module_exit()
 *
 * platform_driver_register()
 * platform_driver_unregister()
 */
module_platform_driver(myirq_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("demo");
MODULE_DESCRIPTION("RK3568 GPIO threaded IRQ example");