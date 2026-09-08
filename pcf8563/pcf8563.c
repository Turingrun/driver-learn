#include "linux/dev_printk.h"
#include "linux/errno.h"
#include "linux/gfp.h"
#include "linux/interrupt.h"
#include "linux/irqreturn.h"
#include "linux/mutex.h"
#include <linux/bcd.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/rtc.h>

#define PCF8563_REG_CTRL2 0x01
#define PCF8563_REG_TIMER_CTRL 0x0E
#define PCF8563_REG_TIMER 0x0F

#define PCF8563_CTRL2_TIE BIT(0)
#define PCF8563_CTRL2_TF BIT(2)

#define PCF8563_TIMER_TE BIT(7)
#define PCF8563_TIMER_1HZ 0x02

struct pcf8563 {
  struct i2c_client *client;
  struct mutex lock;
  int irq;
};

static int pcf8563_read_reg(struct pcf8563 *data, u8 reg, u8 *val) {
  struct i2c_msg msgs[2];
  int ret;

  msgs[0].addr = data->client->addr;
  msgs[0].flags = 0;
  msgs[0].len = 1;
  msgs[0].buf = &reg;

  msgs[1].addr = data->client->addr;
  msgs[1].flags = I2C_M_RD;
  msgs[1].len = 1;
  msgs[1].buf = val;

  mutex_lock(&data->lock);
  ret = i2c_transfer(data->client->adapter, msgs, 2);
  mutex_unlock(&data->lock);

  if (ret < 0)
    return ret;

  if (ret != 2)
    return -EIO;

  return 0;
}

static int pcf8563_write_reg(struct pcf8563 *data, u8 reg, u8 val) {
  u8 buf[2] = {reg, val};
  struct i2c_msg msg;
  int ret;

  msg.addr = data->client->addr;
  msg.flags = 0;
  msg.len = 2;
  msg.buf = buf;

  mutex_lock(&data->lock);
  ret = i2c_transfer(data->client->adapter, &msg, 1);
  mutex_unlock(&data->lock);

  if (ret < 0)
    return ret;

  if (ret != 1)
    return -EIO;

  return 0;
}

static int pcf8563_test_timer_irq(struct pcf8563 *data) {
  int ret;
  u8 ctrl2;

  /*
   * 先读取 Control_status_2。
   */
  ret = pcf8563_read_reg(data, PCF8563_REG_CTRL2, &ctrl2);
  if (ret)
    return ret;

  dev_info(&data->client->dev, "ctrl2 before = 0x%02x\n", ctrl2);

  /*
   * 清 TF，关闭 alarm interrupt，
   * 打开 timer interrupt。
   */
  ctrl2 &= ~PCF8563_CTRL2_TF;
  ctrl2 &= ~BIT(1);           /* AIE = 0 */
  ctrl2 &= ~BIT(4);           /* TI_TP = 0 */
  ctrl2 |= PCF8563_CTRL2_TIE; /* TIE = 1 */

  ret = pcf8563_write_reg(data, PCF8563_REG_CTRL2, ctrl2);
  if (ret)
    return ret;

  /*
   * 倒计时初值 = 5。
   */
  ret = pcf8563_write_reg(data, PCF8563_REG_TIMER, 1);
  if (ret)
    return ret;

  /*
   * TE = 1
   * TD = 10b -> 1 Hz
   */
  ret = pcf8563_write_reg(data, PCF8563_REG_TIMER_CTRL,
                          PCF8563_TIMER_TE | PCF8563_TIMER_1HZ);
  if (ret)
    return ret;

  dev_info(&data->client->dev, "timer irq test started, irq=%d\n",
           data->client->irq);

  return 0;
}

static irqreturn_t pcf8563_irq_thread(int irq, void *dev_id) {
  struct pcf8563 *data = dev_id;
  u8 ctrl2;
  int ret;

  ret = pcf8563_read_reg(data, PCF8563_REG_CTRL2, &ctrl2);
  if (ret) {
    dev_err(&data->client->dev, "failed to read ctrl2: %d\n", ret);
    return IRQ_NONE;
  }

  dev_info(&data->client->dev, "interrupt received: irq=%d ctrl2=0x%02x\n", irq,
           ctrl2);

  if (ctrl2 & PCF8563_CTRL2_TF) {
    dev_info(&data->client->dev, "PCF8563 timer interrupt\n");

    /*
     * 清除 timer flag。
     */
    ctrl2 &= ~PCF8563_CTRL2_TF;

    ret = pcf8563_write_reg(data, PCF8563_REG_CTRL2, ctrl2);
    if (ret)
      dev_err(&data->client->dev, "failed to clear TF: %d\n", ret);
  }

  return IRQ_HANDLED;
}

static int pcf8563_read_time(struct device *dev, struct rtc_time *time) {
  int ret = 0;
  u8 reg = 0x00;
  u8 rx[9];
  struct pcf8563 *data;
  struct i2c_client *client;
  struct i2c_msg msgs[2] = {};
  data = dev_get_drvdata(dev);
  if (!data) {
    return -ENODATA;
  }
  client = to_i2c_client(dev);
  msgs[0].addr = client->addr;
  msgs[0].flags = 0;
  msgs[0].len = 1;
  msgs[0].buf = &reg;

  msgs[1].addr = client->addr;
  msgs[1].flags = I2C_M_RD;
  msgs[1].len = 9;
  msgs[1].buf = rx;

  mutex_lock(&data->lock);

  ret = i2c_transfer(client->adapter, msgs, 2);
  mutex_unlock(&data->lock);
  if (ret < 0) {
    return ret;
  }
  if (ret != 2) {
    ret = -EIO;
    return ret;
  }
  time->tm_sec = bcd2bin(rx[2] & 0x7F);
  time->tm_min = bcd2bin(rx[3] & 0x7F);
  time->tm_hour = bcd2bin(rx[4] & 0x3F);
  time->tm_mday = bcd2bin(rx[5] & 0x3F);
  time->tm_wday = bcd2bin(rx[6] & 0x07);
  time->tm_mon = bcd2bin(rx[7] & 0x1F) - 1;
  time->tm_year = bcd2bin(rx[8]);

  if (time->tm_year < 70)
    time->tm_year += 100;
  return 0;
}

static int pcf8563_set_time(struct device *dev, struct rtc_time *time) {
  int ret = 0;
  u8 tx[8];
  struct pcf8563 *data;
  struct i2c_msg msgs[1] = {};
  data = dev_get_drvdata(dev);
  if (!data) {
    return -ENODATA;
  }
  msgs[0].addr = data->client->addr;
  msgs[0].flags = 0;
  msgs[0].len = 8;
  msgs[0].buf = tx;

  tx[0] = 0x02;
  tx[1] = bin2bcd(time->tm_sec);
  tx[2] = bin2bcd(time->tm_min);
  tx[3] = bin2bcd(time->tm_hour);
  tx[4] = bin2bcd(time->tm_mday);
  tx[5] = bin2bcd(time->tm_wday);
  tx[6] = bin2bcd(time->tm_mon + 1);
  tx[7] = bin2bcd(time->tm_year - 100);

  mutex_lock(&data->lock);
  ret = i2c_transfer(data->client->adapter, msgs, 1);
  mutex_unlock(&data->lock);
  if (ret < 0) {
    return ret;
  }
  if (ret != 1) {
    ret = -EIO;
    return ret;
  }
  return 0;
}

static const struct rtc_class_ops pcf8563_ops = {
    .read_time = pcf8563_read_time,
    .set_time = pcf8563_set_time,
};

static int pcf8563_probe(struct i2c_client *client) {
  int ret;
  struct pcf8563 *data;
  struct rtc_device *rtc_dev;
  struct device *dev = &client->dev;

  if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
    return -ENODEV;
  data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
  if (!data)
    return -ENOMEM;

  data->client = client;

  mutex_init(&data->lock);

  i2c_set_clientdata(client, data);

  dev_info(dev, "client irq = %d\n", client->irq);

  if (client->irq > 0) {
    ret = devm_request_threaded_irq(dev, client->irq, NULL, pcf8563_irq_thread,
                                    IRQF_ONESHOT | IRQF_TRIGGER_LOW, "pcf8563",
                                    data);

    if (ret) {
      dev_err(dev, "failed to request irq %d: %d\n", client->irq, ret);
      return ret;
    }
  }

  /*
   * 实验：启动5秒timer IRQ
   */
  ret = pcf8563_test_timer_irq(data);
  if (ret) {
    dev_err(dev, "failed to start timer irq test: %d\n", ret);
    return ret;
  }

  rtc_dev =
      devm_rtc_device_register(dev, "pcf8563-test", &pcf8563_ops, THIS_MODULE);

  if (IS_ERR(rtc_dev))
    return PTR_ERR(rtc_dev);

  dev_info(dev, "pcf8563 probe success\n");

  return 0;
}

static int pcf8563_remove(struct i2c_client *client) {
  dev_info(&client->dev, "pcf unload");
  return 0;
}

static const struct of_device_id pcf8563_of_match[] = {
    {.compatible = "my,my8563"}, {}};

MODULE_DEVICE_TABLE(of, pcf8563_of_match);
static struct i2c_driver pcf8563_driver = {
    .driver = {.name = "pcf8563", .of_match_table = pcf8563_of_match},
    .probe_new = pcf8563_probe,
    .remove = pcf8563_remove,

};

module_i2c_driver(pcf8563_driver);

MODULE_AUTHOR("Jailin Ma <majialin7@gmail.com>");
MODULE_DESCRIPTION("NXP PCF8563 RTC");
MODULE_LICENSE("GPL");
