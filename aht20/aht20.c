#include <linux/crc8.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>

#define AHT20_INTERVAL 80
#define AHT20_STATUS_BUSY BIT(7)
#define AHT20_STATUS_CALIBRATED BIT(3)
struct aht20 {
  struct i2c_client *client;
  struct mutex lock;
  unsigned long lastupdate;
  u64 temperature;
  u64 humidity;
  u8 valid;
};

static u8 aht20_measure_crc8(u8 *buf, u8 num) {
  u8 i;
  u8 byte;
  u8 crc = 0xFF;
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
  u32 raw_humidity;
  u32 raw_temperature;
  u8 aht20_measure_cmd[3] = {0xAC, 0x33, 0x00};
  u8 rx[7];
  struct aht20 *aht20 = i2c_get_clientdata(client);
  mutex_lock(&aht20->lock);
  ret = i2c_master_send(aht20->client, aht20_measure_cmd,
                        sizeof(aht20_measure_cmd));
  if (ret < 0) {
    goto out;
  }
  if (ret != sizeof(aht20_measure_cmd)) {
    ret = -EIO;
    goto out;
  }
  msleep(AHT20_INTERVAL);
  ret = i2c_master_recv(aht20->client, rx, sizeof(rx));

  if (ret < 0)
    goto out;

  if (ret != sizeof(rx)) {
    ret = -EIO;
    goto out;
  }

  if ((rx[0] & AHT20_STATUS_BUSY)) {
    ret = -EBUSY;
    goto out;
  }

  if (aht20_measure_crc8(rx, 6) != rx[6]) {
    ret = -EBADMSG;
    goto out;
  }

  raw_humidity = ((u32)rx[1] << 12 | (u32)rx[2] << 4 | (u32)rx[3] >> 4);
  raw_temperature = ((u32)(rx[3] & 0x0f) << 16) | ((u32)rx[4] << 8) | rx[5];

  /* hwmon reports temperature in millidegrees Celsius and relative
   * humidity in millipercent.  Cast before multiplying so the scaled
   * intermediate values cannot overflow u32.
   */
  aht20->humidity = (u64)raw_humidity * 100000 / 0x100000;
  aht20->temperature = ((u64)raw_temperature * 200000 / 0x100000) - 50000;

  aht20->valid = 1;
  ret = 0;
  goto out;

out:
  mutex_unlock(&aht20->lock);
  return ret;
};

static umode_t aht20_is_visible(const void *drvdata,
                                enum hwmon_sensor_types type, u32 attr,
                                int channel) {
  switch (type) {
  case hwmon_temp:
    if (attr == hwmon_temp_input)
      return 0444;
    break;

  case hwmon_humidity:
    if (attr == hwmon_humidity_input)
      return 0444;
    break;

  default:
    break;
  }

  return 0;
}

static int aht20_read(struct device *dev, enum hwmon_sensor_types type,
                      u32 attr, int channel, long *val) {
  struct aht20 *aht20 = dev_get_drvdata(dev);
  int ret;

  ret = aht20_update_measurements(aht20->client);
  if (ret)
    return ret;

  switch (type) {
  case hwmon_temp:
    if (attr == hwmon_temp_input) {
      *val = aht20->temperature;
      return 0;
    }
    break;

  case hwmon_humidity:
    if (attr == hwmon_humidity_input) {
      *val = aht20->humidity;
      return 0;
    }
    break;

  default:
    break;
  }

  return -EOPNOTSUPP;
}

static const struct hwmon_channel_info *aht20_info[] = {
    HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
    HWMON_CHANNEL_INFO(humidity, HWMON_H_INPUT), NULL};

static const struct hwmon_ops aht20_hwmon_ops = {
    .is_visible = aht20_is_visible,
    .read = aht20_read,
};

static const struct hwmon_chip_info aht20_chip_info = {
    .ops = &aht20_hwmon_ops,
    .info = aht20_info,
};

static int aht20_probe(struct i2c_client *client) {
  struct device *dev = &client->dev;
  struct device *hwmon_dev;
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

  hwmon_dev = devm_hwmon_device_register_with_info(dev, client->name, aht20,
                                                   &aht20_chip_info, NULL);

  dev_info(&client->dev, "my sensor probe\n");

  dev_info(&client->dev, "i2c addr = 0x%02x\n", client->addr);
  return PTR_ERR_OR_ZERO(hwmon_dev);
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
