// SPDX-License-Identifier: GPL-2.0
#include <linux/mfd/palmas.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>

static int palmas_poweroff_notify(struct sys_off_data *data)
{
	struct palmas *palmas = data->cb_data;
	int ret;

	ret = palmas_update_bits(palmas, PALMAS_PMU_CONTROL_BASE, PALMAS_DEV_CTRL,
				 PALMAS_DEV_CTRL_DEV_ON, 0);
	if (ret) {
		dev_err(palmas->dev, "%s: error writing to palmas pmic: %d\n",
			__func__, ret);
		return NOTIFY_DONE;
	}

	/* give it a little time */
	mdelay(200);

	WARN_ON(1);

	return NOTIFY_DONE;
}

static int palmas_poweroff_probe(struct platform_device *pdev)
{
	struct palmas *palmas = dev_get_drvdata(pdev->dev.parent);

	return devm_register_sys_off_handler(&pdev->dev,
					     SYS_OFF_MODE_POWER_OFF,
					     SYS_OFF_PRIO_HIGH,
					     palmas_poweroff_notify,
					     palmas);
}

static const struct of_device_id of_palmas_poweroff_match[] = {
	{ .compatible = "ti,palmas-pm" },
	{ .compatible = "ti,tps65913-pm" },
	{ .compatible = "ti,tps65914-pm" },
	{ .compatible = "ti,tps80036-pm" },
	{ },
};
MODULE_DEVICE_TABLE(of, of_palmas_poweroff_match);

static struct platform_driver palmas_poweroff_driver = {
	.driver = {
		.name = "palmas-poweroff",
		.of_match_table = of_palmas_poweroff_match,
	},
	.probe = palmas_poweroff_probe,
};
module_platform_driver(palmas_poweroff_driver);

MODULE_AUTHOR("Aaron Kling <webgeek1234@gmail.com>");
MODULE_DESCRIPTION("Palmas poweroff driver");
MODULE_LICENSE("GPL v2");
