#include "linux/dev_printk.h"
#include "linux/errno.h"
#include "linux/gfp.h"
#include "linux/i2c.h"
#include "linux/mutex.h"
#include <linux/bcd.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/rtc.h>

struct pcf8563 {
  struct i2c_client *client;
  struct mutex lock;
};
static int pcf8563_read_time(struct device *dev, struct rtc_time *time) {
  int ret = 0;
  u8 reg = 0x00;
  u8 rx[9];
  struct pcf8563 *data;
  struct i2c_msg msgs[2] = {};
  data = dev_get_drvdata(dev);
  if (!data) {
    return -ENODATA;
  }

  msgs[0].addr = data->client->addr;
  msgs[0].flags = 0;
  msgs[0].len = 1;
  msgs[0].buf = &reg;

  msgs[1].addr = data->client->addr;
  msgs[1].flags = I2C_M_RD;
  msgs[1].len = 9;
  msgs[1].buf = rx;

  mutex_lock(&data->lock);

  ret = i2c_transfer(data->client->adapter, msgs, 2);
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
};

static int pcf8563_set_time(struct device *dev, struct rtc_time *time) {
  int ret = 0;
  u8 tx[8];
  struct pcf8563 *data;
  struct i2c_msg msgs[2] = {};
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

const struct rtc_class_ops pcf8563_ops = {
    .read_time = pcf8563_read_time,
    .set_time = pcf8563_set_time,
};

static int pcf8563_probe(struct i2c_client *client) {
  int ret = 0;
  struct pcf8563 *data;
  struct rtc_device *rtc_dev;
  struct device *dev = &client->dev;
  if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
    ret = -ENOTSUPP;
    goto out;
  }
  data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
  if (!data) {
    ret = -ENOMEM;
    goto out;
  }
  data->client = client;

  mutex_init(&data->lock);
  i2c_set_clientdata(client, data);
  rtc_dev = devm_rtc_device_register(&client->dev, NULL, &pcf8563_ops, NULL);
  dev_info(&client->dev, "pcf probe");

out:
  return ret;
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
