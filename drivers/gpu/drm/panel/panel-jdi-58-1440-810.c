// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * Adapted from the downstream tegradc driver by nvidia
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

struct jdi_panel {
	struct drm_panel base;
	struct mipi_dsi_device *dsi;

	struct regulator *avdd_lcd;
	struct regulator *dvdd_lcd;
	struct backlight_device *backlight;

	struct gpio_desc *enable_gpio;
	struct gpio_desc *reset_gpio;

	const struct drm_display_mode *mode;
};

static inline struct jdi_panel *to_panel_jdi(struct drm_panel *panel)
{
	return container_of(panel, struct jdi_panel, base);
}

static void jdi_wait_frames(struct jdi_panel *jdi, unsigned int frames)
{
	unsigned int refresh = drm_mode_vrefresh(jdi->mode);

	if (WARN_ON(frames > refresh))
		return;

	msleep(1000 / (refresh / frames));
}

static int jdi_panel_disable(struct drm_panel *panel)
{
	struct jdi_panel *jdi = to_panel_jdi(panel);

	backlight_disable(jdi->backlight);

	jdi_wait_frames(jdi, 2);

	return 0;
}

static int jdi_panel_unprepare(struct drm_panel *panel)
{
	struct jdi_panel *jdi = to_panel_jdi(panel);
	int ret;

	ret = mipi_dsi_dcs_set_display_off(jdi->dsi);
	if (ret < 0)
		dev_err(panel->dev, "failed to set display off: %d\n", ret);

	/* Specified by JDI @ 50ms, subject to change */
	msleep(50);

	ret = mipi_dsi_dcs_enter_sleep_mode(jdi->dsi);
	if (ret < 0)
		dev_err(panel->dev, "failed to enter sleep mode: %d\n", ret);

	/* Specified by JDI @ 150ms, subject to change */
	msleep(150);

	gpiod_set_value(jdi->reset_gpio, 1);

	usleep_range(1000, 3000);

	gpiod_set_value(jdi->enable_gpio, 0);

	/* T5 = 2ms */
	usleep_range(3000, 5000);

	regulator_disable(jdi->dvdd_lcd);
	regulator_disable(jdi->avdd_lcd);

	return ret;
}

static int jdi_panel_prepare(struct drm_panel *panel)
{
	struct jdi_panel *jdi = to_panel_jdi(panel);
	int err;
	u8 display_id[3];

	/* Disable backlight to avoid showing random pixels
	 * with a conservative delay for it to take effect.
	 */
	backlight_disable(jdi->backlight);
	jdi_wait_frames(jdi, 3);

	jdi->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	err = regulator_enable(jdi->avdd_lcd);
	if (err < 0) {
		dev_err(panel->dev, "failed to enable avdd_lcd: %d\n", err);
		return err;
	}

	if (jdi->mode->vdisplay == 810)
		regulator_set_voltage(jdi->avdd_lcd, 3100000, 3100000);

	msleep(10);

	err = regulator_enable(jdi->dvdd_lcd);
	if (err < 0) {
		dev_err(panel->dev, "failed to enable dvdd_lcd: %d\n", err);
		goto poweroff;
	}
	msleep(10);

	gpiod_set_value(jdi->enable_gpio, 1);
	msleep(100);

	gpiod_set_value(jdi->reset_gpio, 0);
	msleep(180);

	err = mipi_dsi_set_maximum_return_packet_size(jdi->dsi, 3);
	if (err < 0) {
		dev_err(panel->dev, "failed to set dsi return packet size: %d\n", err);
		goto poweroff;
	}

	// This is primarily to ensure that the panel is responding
	err = mipi_dsi_dcs_read(jdi->dsi, MIPI_DCS_GET_DISPLAY_ID,
				display_id, sizeof(display_id));
	if (err < 0) {
		dev_err(panel->dev, "failed to get display id: %d\n", err);
		goto poweroff;
	}
	dev_info(panel->dev, "display ID[%d]: %02x %02x %02x\n",
		 err, display_id[0], display_id[1], display_id[2]);

	err = mipi_dsi_dcs_set_pixel_format(jdi->dsi, MIPI_DCS_PIXEL_FMT_24BIT);
	if (err < 0) {
		dev_err(panel->dev, "failed to set pixel format: %d\n", err);
		goto poweroff;
	}

	err = mipi_dsi_dcs_exit_sleep_mode(jdi->dsi);
	if (err < 0) {
		dev_err(panel->dev, "failed to exit sleep mode: %d\n", err);
		goto poweroff;
	}

	/*
	 * We need to wait 150ms between mipi_dsi_dcs_exit_sleep_mode() and
	 * mipi_dsi_dcs_set_display_on().
	 */
	msleep(180);

	err = mipi_dsi_dcs_set_display_on(jdi->dsi);
	if (err < 0) {
		dev_err(panel->dev, "failed to set display on: %d\n", err);
		goto poweroff;
	}
	msleep(20);

	jdi->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	return 0;

poweroff:
	if (regulator_is_enabled(jdi->dvdd_lcd))
		regulator_disable(jdi->dvdd_lcd);

	regulator_disable(jdi->avdd_lcd);
	/* Specified by JDI @ 20ms, subject to change */
	msleep(20);

	return err;
}

static int jdi_panel_enable(struct drm_panel *panel)
{
	struct jdi_panel *jdi = to_panel_jdi(panel);

	/*
	 * Ensure we send image data before turning the backlight
	 * on, to avoid the display showing random pixels.
	 */
	jdi_wait_frames(jdi, 3);

	backlight_enable(jdi->backlight);

	return 0;
}

static const struct drm_display_mode jdi_810_mode = {
	.clock = (1440 + 128 + 48 + 48) * (810 + 8 + 8 + 4) * 60 / 1000,
	.hdisplay = 1440,
	.hsync_start = 1440 + 128,
	.hsync_end = 1440 + 128 + 48,
	.htotal = 1440 + 128 + 48 + 48,
	.vdisplay = 810,
	.vsync_start = 810 + 8,
	.vsync_end = 810 + 8 + 8,
	.vtotal = 810 + 8 + 8 + 4,
	.flags = 0,
};

static const struct drm_display_mode jdi_720_mode = {
	.clock = (720 + 112 + 30 + 32) * (1280 + 11 + 11 + 2) * 60 / 1000,
	.hdisplay = 720,
	.hsync_start = 720 + 112,
	.hsync_end = 720 + 112 + 30,
	.htotal = 720 + 112 + 30 + 32,
	.vdisplay = 1280,
	.vsync_start = 1280 + 11,
	.vsync_end = 1280 + 11 + 11,
	.vtotal = 1280 + 11 + 11 + 2,
	.flags = 0,
};

static int jdi_panel_get_modes(struct drm_panel *panel,
			       struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	struct jdi_panel *jdi = to_panel_jdi(panel);
	struct device *dev = &jdi->dsi->dev;

	mode = drm_mode_duplicate(connector->dev, jdi->mode);
	if (!mode) {
		dev_err(dev, "failed to add mode %ux%ux@%u\n",
			jdi->mode->hdisplay, jdi->mode->vdisplay,
			drm_mode_vrefresh(jdi->mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = 130;
	connector->display_info.height_mm = 74;
	connector->display_info.bpc = 8;

	return 1;
}

static const struct drm_panel_funcs jdi_panel_funcs = {
	.prepare = jdi_panel_prepare,
	.enable = jdi_panel_enable,
	.disable = jdi_panel_disable,
	.unprepare = jdi_panel_unprepare,
	.get_modes = jdi_panel_get_modes,
};

static const struct of_device_id jdi_of_match[] = {
	{ .compatible = "jdi,58-1440-810", &jdi_810_mode },
	{ .compatible = "jdi,50-720-1280", &jdi_720_mode },
	{ }
};
MODULE_DEVICE_TABLE(of, jdi_of_match);

static int jdi_panel_add(struct jdi_panel *jdi)
{
	struct device *dev = &jdi->dsi->dev;

	jdi->avdd_lcd = devm_regulator_get(dev, "avdd_lcd");
	if (IS_ERR(jdi->avdd_lcd))
		return dev_err_probe(dev, PTR_ERR(jdi->avdd_lcd),
				     "failed to get avdd_lcd regulator\n");

	jdi->dvdd_lcd = devm_regulator_get(dev, "dvdd_lcd");
	if (IS_ERR(jdi->dvdd_lcd))
		return dev_err_probe(dev, PTR_ERR(jdi->dvdd_lcd),
				     "failed to get dvdd_lcd regulator\n");

	jdi->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(jdi->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(jdi->reset_gpio),
				     "failed to get reset gpio\n");
	/* T4 = 1ms */
	usleep_range(1000, 3000);

	jdi->enable_gpio = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(jdi->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(jdi->enable_gpio),
				     "failed to get enable gpio\n");
	/* T5 = 2ms */
	usleep_range(2000, 4000);

	jdi->backlight = devm_of_find_backlight(dev);
	if (IS_ERR(jdi->backlight))
		return dev_err_probe(dev, PTR_ERR(jdi->backlight),
				     "failed to create backlight\n");

	return devm_drm_panel_add(dev, &jdi->base);
}

static int jdi_panel_dsi_probe(struct mipi_dsi_device *dsi)
{
	struct jdi_panel *jdi;
	int err;

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_VIDEO_SYNC_PULSE | MIPI_DSI_MODE_NO_EOT_PACKET;

	jdi = devm_drm_panel_alloc(&dsi->dev, __typeof(*jdi), base,
				   &jdi_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(jdi))
		return PTR_ERR(jdi);

	mipi_dsi_set_drvdata(dsi, jdi);

	jdi->dsi = dsi;
	jdi->mode = of_device_get_match_data(&dsi->dev);

	err = jdi_panel_add(jdi);
	if (err < 0)
		return err;

	return devm_mipi_dsi_attach(&dsi->dev, dsi);
}

static void jdi_panel_dsi_remove(struct mipi_dsi_device *dsi)
{
	struct jdi_panel *jdi = mipi_dsi_get_drvdata(dsi);
	int err;

	err = jdi_panel_disable(&jdi->base);
	if (err < 0)
		dev_err(&dsi->dev, "failed to disable panel: %d\n", err);
}

static void jdi_panel_dsi_shutdown(struct mipi_dsi_device *dsi)
{
	struct jdi_panel *jdi = mipi_dsi_get_drvdata(dsi);

	if (!jdi)
		return;

	jdi_panel_disable(&jdi->base);
}

static struct mipi_dsi_driver jdi_panel_dsi_driver = {
	.driver = {
		.name = "panel-jdi-58-1440-810",
		.of_match_table = jdi_of_match,
	},
	.probe = jdi_panel_dsi_probe,
	.remove = jdi_panel_dsi_remove,
	.shutdown = jdi_panel_dsi_shutdown,
};
module_mipi_dsi_driver(jdi_panel_dsi_driver);

MODULE_AUTHOR("Aaron Kling <webgeek1234@gmail.com>");
MODULE_DESCRIPTION("DRM Driver for JDI 1440x810 DSI panel, command mode");
MODULE_LICENSE("GPL");
