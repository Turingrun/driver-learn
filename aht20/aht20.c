#include "linux/delay.h"
#include "linux/dev_printk.h"
#include "linux/i2c.h"
#include "linux/string.h"
#include "linux/types.h"
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>

#define AHT20_INTERVAL 80
#define AHT20_DETECT 0x98

struct aht20 {
  struct i2c_client *client;
  struct mutex lock;
  unsigned long lastupdate;
  int temperature;
  int humidity;
  uint8_t valid;
};

static uint8_t aht20_measure_crc8(uint8_t *buf, uint8_t num) {
  uint8_t i;
  uint8_t byte;
  uint8_t crc = 0xFF;
  for (byte = 0; byte < num; byte++) {
    crc ^= (buf[byte]);
    for (i = 8; i > 0; --i) {
      if (crc & 0x80)
        crc = (crc << 1) ^ 0x31;
      else
        crc = (crc << 1);
    }
  }
  return crc;
}

static int aht20_update_measurements(struct i2c_client *client) {
  int ret = 0;
  uint32_t data = 0;
  uint8_t aht20_measure_cmd[3] = {0xAC, 0x33, 0x00};
  uint8_t rx[7];
  struct aht20 *aht20 = i2c_get_clientdata(client);
  mutex_lock(&aht20->lock);
  ret = i2c_master_send(aht20->client, aht20_measure_cmd, 3);
  if (ret < 0) {
    goto fail;
  }
  msleep(AHT20_INTERVAL);
  ret = i2c_master_recv(aht20->client, rx, 7);
  if (ret != 7) {
    goto fail;
  }
  if (aht20_measure_crc8(rx, 6) != rx[6]) {
    goto fail;
  }
  if ((rx[0] & AHT20_DETECT) != 0x18) {
    goto fail;
  }
  memcpy(&data, rx + 1, 3);
  aht20->humidity = ((data >> 4) / 0x100000) * 100;
  memcpy(&data, rx + 3, 3);
  aht20->temperature = ((data & 0xFFFFF) / 0x100000) * 200 - 50;
  aht20->valid = 1;
fail: {
  aht20->valid = 0;
  mutex_unlock(&aht20->lock);
}
  return 0;
};

static int aht20_probe(struct i2c_client *client) {
  struct device *dev = &client->dev;
  struct aht20 *aht20;
  if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
    return -EOPNOTSUPP;
  }

  aht20 = devm_kzalloc(dev, sizeof(*aht20), GFP_KERNEL);
  if (!aht20)
    return -ENOMEM;

  aht20->client = client;
  mutex_init(&aht20->lock);

  i2c_set_clientdata(client, aht20);

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