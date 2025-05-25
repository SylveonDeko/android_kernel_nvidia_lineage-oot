// SPDX-License-Identifier: GPL-2.0+
/*
 * Battery driver for On semi lc709203f
 *
 * Based on downstream driver from NVIDIA
 *
 * Copyright (c) 2014-2015, NVIDIA CORPORATION.  All rights reserved.
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/power_supply.h>

#define LC709203F_THERMISTOR_B		0x06
#define LC709203F_VOLTAGE		0x09
#define LC709203F_RSOC			0x0D
#define LC709203F_ALARM_LOW_CELL_RSOC	0x13
#define LC709203F_ALARM_LOW_CELL_VOLT	0x14
#define LC709203F_STATUS_BIT		0x16
#define LC709203F_NUM_OF_THE_PARAM	0x1A

#define LC709203F_BATTERY_LOW		15
#define LC709203F_BATTERY_FULL		100

struct lc709203f_platform_data {
	const char *tz_name;
	u32 thermistor_beta;
	u32 therm_adjustment;
	u32 threshold_soc;
	u32 maximum_soc;
	u32 alert_low_rsoc;
	u32 alert_low_voltage;
	bool support_battery_current;
};

struct lc709203f_chip {
	struct i2c_client *client;
	struct delayed_work work;
	struct power_supply *battery;
	struct lc709203f_platform_data *pdata;

	int vcell;
	int soc;
	int status;
	int health;
	int capacity_level;

	int temperature;

	int lasttime_soc;
	int lasttime_status;
	int charge_complete;
	struct mutex mutex;
	int read_failed;
};

static const enum power_supply_property lc709203f_battery_properties[] = {
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
};

static int lc709203f_get_adjusted_soc(int min_soc, int max_soc, int actual_soc_semi)
{
        int min_soc_semi = min_soc * 100;
        int max_soc_semi = max_soc * 100;

        if (actual_soc_semi >= max_soc_semi)
                return 100;

        if (actual_soc_semi <= min_soc_semi)
                return 0;

        return (actual_soc_semi - min_soc_semi + 50) / (max_soc - min_soc);
}

static int lc709203f_update_soc_voltage(struct lc709203f_chip *chip)
{
	int val;

	val = i2c_smbus_read_word_data(chip->client, LC709203F_VOLTAGE);
	if (val < 0)
		dev_err(&chip->client->dev, "%s: err %d\n", __func__, val);
	else
		chip->vcell = val;

	val = i2c_smbus_read_word_data(chip->client, LC709203F_RSOC);
	if (val < 0)
		dev_err(&chip->client->dev, "%s: err %d\n", __func__, val);
	else
		chip->soc = lc709203f_get_adjusted_soc(
				chip->pdata->threshold_soc,
				chip->pdata->maximum_soc, val * 100);

	if (chip->soc == LC709203F_BATTERY_FULL && chip->charge_complete) {
		chip->status = POWER_SUPPLY_STATUS_FULL;
		chip->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
		chip->health = POWER_SUPPLY_HEALTH_GOOD;
	} else if (chip->soc < LC709203F_BATTERY_LOW) {
		chip->status = chip->lasttime_status;
		chip->health = POWER_SUPPLY_HEALTH_DEAD;
		chip->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
	} else {
		chip->charge_complete = 0;
		chip->status = chip->lasttime_status;
		chip->health = POWER_SUPPLY_HEALTH_GOOD;
		chip->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
	}

	return 0;
}

static int lc709203f_get_property(struct power_supply *psy,
				  enum power_supply_property psp,
				  union power_supply_propval *val)
{
	struct lc709203f_chip *chip = power_supply_get_drvdata(psy);

	mutex_lock(&chip->mutex);

	switch (psp) {
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = chip->soc;
		break;

	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		val->intval = chip->capacity_level;
		break;

	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = chip->health;
		break;

	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;

	case POWER_SUPPLY_PROP_STATUS:
		val->intval = chip->status;
		break;

	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = 1000 * chip->vcell;
		break;

	default:
		dev_err(&chip->client->dev, "%s: invalid property %u\n", __func__, psp);
		mutex_unlock(&chip->mutex);
		return -EINVAL;
	}

	mutex_unlock(&chip->mutex);

	return 0;
}

static void lc709203f_work(struct work_struct *work)
{
	struct lc709203f_chip *chip;

	chip = container_of(work, struct lc709203f_chip, work.work);

	mutex_lock(&chip->mutex);

	lc709203f_update_soc_voltage(chip);

	if (chip->soc != chip->lasttime_soc ||
		chip->status != chip->lasttime_status) {
		chip->lasttime_soc = chip->soc;
		power_supply_changed(chip->battery);
	}

	mutex_unlock(&chip->mutex);

	/* continuously send uevent notification */
	schedule_delayed_work(&chip->work, 30 * HZ);
}

static irqreturn_t lc709203f_irq(int id, void *dev)
{
	struct lc709203f_chip *chip = dev;
	struct i2c_client *client = chip->client;

	dev_info(&client->dev, "%s(): STATUS_VL\n", __func__);
	/* Forced set SOC 0 to power off */
	chip->soc = 0;
	chip->lasttime_soc = chip->soc;
	chip->status = chip->lasttime_status;
	chip->health = POWER_SUPPLY_HEALTH_DEAD;
	chip->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
	power_supply_changed(chip->battery);

	return IRQ_HANDLED;
}

static void lc709203f_parse_platform_data(struct i2c_client *client,
					  struct lc709203f_platform_data *pdata)
{
	device_property_read_u32(&client->dev, "onsemi,thermistor-beta", &pdata->thermistor_beta);
	device_property_read_u32(&client->dev, "onsemi,thermistor-adjustment", &pdata->therm_adjustment);
	device_property_read_u32(&client->dev, "onsemi,onsemi,kernel-threshold-soc", &pdata->threshold_soc);
	device_property_read_u32(&client->dev, "onsemi,alert-low-rsoc", &pdata->alert_low_rsoc);
	device_property_read_u32(&client->dev, "onsemi,onsemi,alert-low-voltage", &pdata->alert_low_voltage);

	if (device_property_read_u32(&client->dev, "onsemi,kernel-maximum-soc", &pdata->maximum_soc))
		pdata->maximum_soc = 100;

}

static const struct power_supply_desc lc709203f_battery_desc = {
	.name = "lc709203f-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = lc709203f_battery_properties,
	.get_property = lc709203f_get_property,
	.num_properties = ARRAY_SIZE(lc709203f_battery_properties),
	.external_power_changed = power_supply_changed,
};

static int lc709203f_battery_i2c_probe(struct i2c_client *client)
{
	struct power_supply_config psy_cfg = {};
	struct lc709203f_chip *chip;
	int ret;

	/* Required PEC functionality */
	client->flags |= I2C_CLIENT_PEC;

	/* Check if device exist or not */
	ret = i2c_smbus_read_word_data(client, LC709203F_NUM_OF_THE_PARAM);
	if (ret < 0) {
		dev_err(&client->dev, "device is not responding, %d\n", ret);
		return ret;
	}

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	if (client->dev.of_node) {
		psy_cfg.of_node = client->dev.of_node;
		chip->pdata = devm_kzalloc(&client->dev,
					sizeof(*chip->pdata), GFP_KERNEL);
		if (chip->pdata)
			lc709203f_parse_platform_data(client, chip->pdata);
	} else
		chip->pdata = client->dev.platform_data;

	if (!chip->pdata)
		return -ENODATA;

	chip->client = client;
	i2c_set_clientdata(client, chip);
	psy_cfg.drv_data = chip;

	ret = i2c_smbus_write_word_data(chip->client,
		LC709203F_ALARM_LOW_CELL_RSOC, chip->pdata->alert_low_rsoc);
	if (ret < 0) {
		dev_err(&client->dev, "LOW_RSOC write failed: %d\n", ret);
		return ret;
	}

	ret = i2c_smbus_write_word_data(chip->client,
		LC709203F_ALARM_LOW_CELL_VOLT, chip->pdata->alert_low_voltage);
	if (ret < 0) {
		dev_err(&client->dev, "LOW_VOLT write failed: %d\n", ret);
		return ret;
	}

	if (chip->pdata->thermistor_beta) {
		ret = i2c_smbus_write_word_data(chip->client,
			LC709203F_THERMISTOR_B, chip->pdata->thermistor_beta);
		if (ret < 0) {
			dev_err(&client->dev, "THERMISTOR_B write failed: %d\n", ret);
			return ret;
		}

		ret = i2c_smbus_write_word_data(chip->client, LC709203F_STATUS_BIT, 0x1);
		if (ret < 0) {
			dev_err(&client->dev, "STATUS_BIT write failed: %d\n", ret);
			return ret;
		}
	}

	lc709203f_update_soc_voltage(chip);

	chip->battery = devm_power_supply_register(&client->dev, &lc709203f_battery_desc, &psy_cfg);
	if (IS_ERR(chip->battery))
		return dev_err_probe(&client->dev, IS_ERR(chip->battery),
				     "failed to register battery\n");

	mutex_init(&chip->mutex);

	INIT_DEFERRABLE_WORK(&chip->work, lc709203f_work);
	schedule_delayed_work(&chip->work, 0);

	if (client->irq) {
		ret = devm_request_threaded_irq(&client->dev, client->irq,
			NULL, lc709203f_irq,
			IRQF_ONESHOT | IRQF_TRIGGER_FALLING,
			dev_name(&client->dev), chip);
		if (ret < 0) {
			dev_err(&client->dev,
				"%s: request IRQ %d fail, err = %d\n",
				__func__, client->irq, ret);
			client->irq = 0;
			goto irq_reg_error;
		}
	}

	device_set_wakeup_capable(&client->dev, 1);

	return 0;

irq_reg_error:
	cancel_delayed_work_sync(&chip->work);
	mutex_destroy(&chip->mutex);

	return ret;
}

static void lc709203f_battery_i2c_remove(struct i2c_client *client)
{
	struct lc709203f_chip *chip = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&chip->work);
	mutex_destroy(&chip->mutex);
}

static int __maybe_unused lc709203f_battery_suspend(struct device *dev)
{
	struct lc709203f_chip *chip = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&chip->work);

	if (device_may_wakeup(&chip->client->dev))
		enable_irq_wake(chip->client->irq);

	return 0;
}

static int __maybe_unused lc709203f_battery_resume(struct device *dev)
{
	struct lc709203f_chip *chip = dev_get_drvdata(dev);

	if (device_may_wakeup(&chip->client->dev))
		disable_irq_wake(chip->client->irq);

	mutex_lock(&chip->mutex);
	lc709203f_update_soc_voltage(chip);
	power_supply_changed(chip->battery);
	mutex_unlock(&chip->mutex);

	schedule_delayed_work(&chip->work, HZ);

	return 0;
}

static SIMPLE_DEV_PM_OPS(lc709203f_battery_pm_ops,
			 lc709203f_battery_suspend, lc709203f_battery_resume);

static const struct i2c_device_id lc709203f_i2c_id_table[] = {
	{ "lc709203f", 0 },
	{},
};
MODULE_DEVICE_TABLE(i2c, lc709203f_i2c_id_table);

#ifdef CONFIG_OF
static const struct of_device_id lc709203f_battery_i2c_of_match_table[] = {
	{ .compatible = "onsemi,lc709203f" },
	{},
};
MODULE_DEVICE_TABLE(of, lc709203f_battery_i2c_of_match_table);
#endif

static struct i2c_driver lc709203f_battery_i2c_driver = {
	.driver = {
		.name = "lc709203f-battery",
		.of_match_table = of_match_ptr(lc709203f_battery_i2c_of_match_table),
		.pm = &lc709203f_battery_pm_ops,
	},
	.probe = lc709203f_battery_i2c_probe,
	.remove = lc709203f_battery_i2c_remove,
	.id_table = lc709203f_i2c_id_table,
};
module_i2c_driver(lc709203f_battery_i2c_driver);

MODULE_AUTHOR("Chaitanya Bandi <bandik@nvidia.com>");
MODULE_DESCRIPTION("OnSemi LC709203F Fuel Gauge");
MODULE_LICENSE("GPL v2");
