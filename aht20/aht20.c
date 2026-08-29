#include "linux/dev_printk.h"
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>

struct aht20 {
  struct i2c_client *client;
  struct mutex lock;
  unsigned long lastupdate;
  int temperature;
  int humidity;
};

static int aht20_probe(struct i2c_client *client) {
  struct device *dev = &client->dev;
  struct aht20 *aht20;
  aht20 = devm_kzalloc(dev, sizeof(*aht20), GFP_KERNEL);
  if (!aht20)
    return -ENOMEM;

  aht20->client = client;
  mutex_init(&aht20->lock);
  dev_info(&client->dev, "my sensor probe\n");

  dev_info(&client->dev, "i2c addr = 0x%02x\n", client->addr);
  return 0;
};
static int aht20_remove(struct i2c_client *client) {
  dev_info(&client->dev, "my sensor remove\n");
  return 0;
};

static const struct of_device_id aht20_of_match[] = {
    {.compatible = "aosong,aht20"}, {}};

MODULE_DEVICE_TABLE(of, aht20_of_match);

static struct i2c_driver aht20_driver = {
    .driver =
        {
            .name = "aht20",
            .of_match_table = aht20_of_match,
        },
    .probe_new = aht20_probe,
    .remove = aht20_remove,
};

module_i2c_driver(aht20_driver);

MODULE_AUTHOR("Jailin Ma <majialin7@gmail.com>");
MODULE_DESCRIPTION("AoSong Aht20 humidity and temperature sensor driver");
MODULE_LICENSE("GPL");