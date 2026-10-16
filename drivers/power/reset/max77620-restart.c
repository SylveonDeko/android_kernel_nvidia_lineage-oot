// SPDX-License-Identifier: GPL-2.0
#include <linux/mfd/max77620.h>
#include <linux/delay.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/reboot.h>
#include <linux/regmap.h>

#define MAX77620_ONOFFCNFG2_WK_ALARM2	BIT(1)

static int max77620_restart_notify(struct sys_off_data *data)
{
	struct max77620_chip *max77620 = data->cb_data;
	int ret;

	ret = regmap_update_bits(max77620->rmap, MAX77620_REG_ONOFFCNFG2,
				 MAX77620_ONOFFCNFG2_SFT_RST_WK,
				 MAX77620_ONOFFCNFG2_SFT_RST_WK);
	if (ret < 0) {
		dev_err(max77620->dev, "%s: failed to set SFT_RST_WK: %d\n",
			__func__, ret);
		return NOTIFY_DONE;
	}

	ret = regmap_update_bits(max77620->rmap, MAX77620_REG_ONOFFCNFG1,
				 MAX77620_ONOFFCNFG1_SFT_RST,
				 MAX77620_ONOFFCNFG1_SFT_RST);
	if (ret < 0) {
		dev_err(max77620->dev, "%s: failed to set SFT_RST: %d\n",
			__func__, ret);
		return NOTIFY_DONE;
	}

	/* give it a little time */
	mdelay(200);

	WARN_ON(1);

	return NOTIFY_DONE;
}

static int max77620_power_off_notify(struct sys_off_data *data)
{
	struct max77620_chip *max77620 = data->cb_data;
	int ret;

	ret = regmap_update_bits(max77620->rmap, MAX77620_REG_ONOFFCNFG2,
				 MAX77620_ONOFFCNFG2_SFT_RST_WK |
				 MAX77620_ONOFFCNFG2_WK_ALARM1 |
				 MAX77620_ONOFFCNFG2_WK_ALARM2, 0);
	if (ret < 0) {
		dev_err(max77620->dev, "%s: failed to clear wake events: %d\n",
			__func__, ret);
		return NOTIFY_DONE;
	}

	ret = regmap_update_bits(max77620->rmap, MAX77620_REG_ONOFFCNFG1,
				 MAX77620_ONOFFCNFG1_SFT_RST,
				 MAX77620_ONOFFCNFG1_SFT_RST);
	if (ret < 0) {
		dev_err(max77620->dev, "%s: failed to set SFT_RST: %d\n",
			__func__, ret);
		return NOTIFY_DONE;
	}

	mdelay(200);

	WARN_ON(1);

	return NOTIFY_DONE;
}

static int max77620_restart_probe(struct platform_device *pdev)
{
	struct max77620_chip *max77620 = dev_get_drvdata(pdev->dev.parent);
	int ret;

	ret = devm_register_sys_off_handler(&pdev->dev,
					    SYS_OFF_MODE_POWER_OFF,
					    SYS_OFF_PRIO_HIGH,
					    max77620_power_off_notify,
					    max77620);
	if (ret)
		return ret;

	if (!device_property_read_bool(pdev->dev.parent, "system-power-controller"))
		return 0;

	return devm_register_sys_off_handler(&pdev->dev,
					     SYS_OFF_MODE_RESTART,
					     SYS_OFF_PRIO_HIGH,
					     max77620_restart_notify,
					     max77620);
}

static const struct platform_device_id max77620_restart_id_table[] = {
	{ "max77620-power", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(platform, max77620_restart_id_table);

static struct platform_driver max77620_restart_driver = {
	.driver = {
		.name = "max77620-power",
	},
	.probe = max77620_restart_probe,
	.id_table = max77620_restart_id_table,
};
module_platform_driver(max77620_restart_driver);

MODULE_AUTHOR("Aaron Kling <webgeek1234@gmail.com>");
MODULE_DESCRIPTION("Maxim MAX77620 PMIC restart driver");
MODULE_LICENSE("GPL v2");
