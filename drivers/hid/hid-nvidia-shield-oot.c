// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.  All rights reserved.
 *
 *  HID driver for NVIDIA SHIELD peripherals.
 */

#include <linux/hid.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/usb.h>

#define USB_VENDOR_ID_NVIDIA			0x0955
#define USB_DEVICE_ID_NVIDIA_LOKI_CONTROLLER	0x7205


#define android_map_key(c) hid_map_usage(hi, usage, bit, max, EV_KEY, (c))

enum {
	HID_USAGE_ANDROID_PLAYPAUSE_BTN = 0xcd, /* Double-tap volume slider */
	HID_USAGE_ANDROID_VOLUMEUP_BTN = 0xe9,
	HID_USAGE_ANDROID_VOLUMEDOWN_BTN = 0xea,
	HID_USAGE_ANDROID_SEARCH_BTN = 0x221, /* NVIDIA btn on Thunderstrike */
	HID_USAGE_ANDROID_HOME_BTN = 0x223,
	HID_USAGE_ANDROID_BACK_BTN = 0x224,
};

/* Common struct for shield accessories. */
struct shield_device {
	struct hid_device *hdev;
};

struct loki_device {
	struct shield_device base;

	/* Sub-devices */
	struct led_classdev power_led;
	struct led_classdev buttons_led;

	/* Resources */
	uint8_t power_brightness;
	uint8_t buttons_brightness;
};

#define UC_COMM_ON	0x10
#define UC_COMM_OFF	0x13
#define UC_COMM_TCH_LED	0x80
static void loki_led_set_brightness(struct led_classdev *led,
				    enum led_brightness value)
{
	struct hid_device *hdev = to_hid_device(led->dev->parent);
	struct usb_interface *intf = to_usb_interface(hdev->dev.parent);
	struct usb_device *udev = interface_to_usbdev(intf);
	uint8_t state = (value ? UC_COMM_ON : UC_COMM_OFF);
	struct loki_device *loki = container_of(hid_get_drvdata(hdev), struct loki_device, base);

	if (led == &loki->buttons_led)
		state |= UC_COMM_TCH_LED;

	usb_control_msg(udev, usb_sndctrlpipe(udev, 0), 0x00, 0x42,
			cpu_to_le16(value), cpu_to_le16(state),
			NULL, 0, 2000);
}

static inline int loki_led_create(struct loki_device *loki)
{
	struct led_classdev *led1 = &loki->power_led;
	struct led_classdev *led2 = &loki->buttons_led;
	int ret;

	led1->name = devm_kasprintf(&loki->base.hdev->dev, GFP_KERNEL,
				    "loki_input:green:power");
	if (!led1->name)
		return -ENOMEM;
	led1->brightness = 255;
	led1->max_brightness = 255;
	led1->flags = LED_CORE_SUSPENDRESUME;
	led1->brightness_set = &loki_led_set_brightness;

	ret = led_classdev_register(&loki->base.hdev->dev, led1);
	if (ret)
		return ret;

	led2->name = devm_kasprintf(&loki->base.hdev->dev, GFP_KERNEL,
				    "loki_input:white:kbd_backlight");
	if (!led2->name)
		return -ENOMEM;
	led2->brightness = 255;
	led2->max_brightness = 255;
	led2->flags = LED_CORE_SUSPENDRESUME;
	led2->brightness_set = &loki_led_set_brightness;

	return led_classdev_register(&loki->base.hdev->dev, led2);
}

static struct shield_device *loki_create(struct hid_device *hdev)
{
	struct shield_device *shield_dev;
	struct loki_device *loki;
	int ret;

	loki = devm_kzalloc(&hdev->dev, sizeof(*loki), GFP_KERNEL);
	if (!loki)
		return ERR_PTR(-ENOMEM);

	shield_dev = &loki->base;
	shield_dev->hdev = hdev;

	hid_set_drvdata(hdev, shield_dev);

	ret = loki_led_create(loki);
	if (ret) {
		hid_err(hdev, "Failed to create Loki LED instance\n");
		return ERR_PTR(ret);
	}

	hid_info(hdev, "Registered Loki controller\n");
	return shield_dev;
}

static void loki_destroy(struct loki_device *loki)
{
	led_classdev_unregister(&loki->power_led);
	led_classdev_unregister(&loki->buttons_led);
}

static int shield_input_mapped(struct hid_device *hdev, struct hid_input *hi,
			       struct hid_field *field, struct hid_usage *usage,
			       unsigned long **bit, int *max)
{
	int fuzz;
	int flat;

	if ((usage->type != EV_ABS) ||
	    !(field->application == HID_GD_GAMEPAD ||
	      field->application == HID_GD_JOYSTICK))
		return 0;

	switch (usage->hid) {
	case HID_GD_X:
	case HID_GD_Y:
	case HID_GD_RX:
	case HID_GD_RY:
		fuzz = 64;
		flat = 64;
		break;
	case HID_GD_Z:
	case HID_GD_RZ:
		fuzz = 64;
		flat = 0;
		break;
	default:
		return 0;
	}

	set_bit(usage->type, hi->input->evbit);
	set_bit(usage->code, *bit);
	input_set_abs_params(hi->input, usage->code, field->logical_minimum,
			     field->logical_maximum, fuzz, flat);
	input_abs_set_res(hi->input, usage->code, hidinput_calc_abs_res(field, usage->code));

	return -1;
}

static int android_input_mapping(struct hid_device *hdev, struct hid_input *hi,
				 struct hid_field *field,
				 struct hid_usage *usage, unsigned long **bit,
				 int *max)
{
	if ((usage->hid & HID_USAGE_PAGE) != HID_UP_CONSUMER)
		return 0;

	switch (usage->hid & HID_USAGE) {
	case HID_USAGE_ANDROID_PLAYPAUSE_BTN:
		android_map_key(KEY_PLAYPAUSE);
		break;
	case HID_USAGE_ANDROID_VOLUMEUP_BTN:
		android_map_key(KEY_VOLUMEUP);
		break;
	case HID_USAGE_ANDROID_VOLUMEDOWN_BTN:
		android_map_key(KEY_VOLUMEDOWN);
		break;
	case HID_USAGE_ANDROID_SEARCH_BTN:
		android_map_key(BTN_Z);
		break;
	case HID_USAGE_ANDROID_HOME_BTN:
		android_map_key(BTN_MODE);
		break;
	case HID_USAGE_ANDROID_BACK_BTN:
		android_map_key(BTN_SELECT);
		break;
	default:
		return 0;
	}

	return 1;
}

static int shield_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct shield_device *shield_dev = NULL;
	int ret;

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "Parse failed\n");
		return ret;
	}

	switch (id->product) {
	case USB_DEVICE_ID_NVIDIA_LOKI_CONTROLLER:
		shield_dev = loki_create(hdev);
		break;
	}

	if (unlikely(!shield_dev)) {
		hid_err(hdev, "Failed to identify SHIELD device\n");
		return -ENODEV;
	}
	if (IS_ERR(shield_dev)) {
		hid_err(hdev, "Failed to create SHIELD device\n");
		return PTR_ERR(shield_dev);
	}

	ret = hid_hw_start(hdev, HID_CONNECT_HIDINPUT);
	if (ret) {
		hid_err(hdev, "Failed to start HID device\n");
		goto err_create;
	}

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "Failed to open HID device\n");
		goto err_stop;
	}

	return ret;

err_stop:
	hid_hw_stop(hdev);
err_create:
	switch (id->product) {
	case USB_DEVICE_ID_NVIDIA_LOKI_CONTROLLER:
		loki_destroy(container_of(hid_get_drvdata(hdev), struct loki_device, base));
		break;
	}
	return ret;
}

static void shield_remove(struct hid_device *hdev)
{
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id shield_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_NVIDIA,
			 USB_DEVICE_ID_NVIDIA_LOKI_CONTROLLER) },
	{ }
};
MODULE_DEVICE_TABLE(hid, shield_devices);

static struct hid_driver shield_driver = {
	.name          = "shield-oot",
	.id_table      = shield_devices,
	.input_mapped  = shield_input_mapped,
	.input_mapping = android_input_mapping,
	.probe         = shield_probe,
	.remove        = shield_remove,
};
module_hid_driver(shield_driver);

MODULE_AUTHOR("Aaron Kling <webgeek1234@gmail.com>");
MODULE_DESCRIPTION("OOT HID Driver for NVIDIA SHIELD peripherals.");
MODULE_LICENSE("GPL");
