/*
 *  Backlight Driver for Sharp Zaurus Handhelds (various models)
 *
 *  Copyright (c) 2004-2006 Richard Purdie
 *
 *  Based on Sharp's 2.4 Backlight Driver
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/fb.h>
#include <linux/backlight.h>
#include <asm/arch/sharpsl.h>
#include <asm/hardware/sharpsl_pm.h>

static int corgibl_intensity;
static DEFINE_MUTEX(bl_mutex);
static struct backlight_properties corgibl_data;
static struct backlight_device *corgi_backlight_device;
static struct corgibl_machinfo *bl_machinfo;

static unsigned long corgibl_flags;
#define CORGIBL_SUSPENDED     0x01
#define CORGIBL_BATTLOW       0x02

static int corgibl_send_intensity(struct backlight_device *bd)
{
	void (*corgi_kick_batt)(void);
	int intensity = bd->props->brightness;

	if (bd->props->power != FB_BLANK_UNBLANK)
		intensity = 0;
	if (bd->props->fb_blank != FB_BLANK_UNBLANK)
		intensity = 0;
	if (corgibl_flags & CORGIBL_SUSPENDED)
		intensity = 0;
	if (corgibl_flags & CORGIBL_BATTLOW)
		intensity &= bl_machinfo->limit_mask;

 	mutex_lock(&bl_mutex);
	bl_machinfo->set_bl_intensity(intensity);
	mutex_unlock(&bl_mutex);

	corgibl_intensity = intensity;

 	corgi_kick_batt = symbol_get(sharpsl_battery_kick);
 	if (corgi_kick_batt) {
 		corgi_kick_batt();
 		symbol_put(sharpsl_battery_kick);
 	}

	return 0;
}

#ifdef CONFIG_PM
static int corgibl_suspend(struct platform_device *dev, pm_message_t state)
{
	corgibl_flags |= CORGIBL_SUSPENDED;
	corgibl_send_intensity(corgi_backlight_device);
	return 0;
}

static int corgibl_resume(struct platform_device *dev)
{
	corgibl_flags &= ~CORGIBL_SUSPENDED;
	corgibl_send_intensity(corgi_backlight_device);
	return 0;
}
#else
#define corgibl_suspend	NULL
#define corgibl_resume	NULL
#endif

static int corgibl_get_intensity(struct backlight_device *bd)
{
	return corgibl_intensity;
}

static int corgibl_set_intensity(struct backlight_device *bd)
{
	corgibl_send_intensity(corgi_backlight_device);
	return 0;
}

/*
 * Called when the battery is low to limit the backlight intensity.
 * If limit==0 clear any limit, otherwise limit the intensity
 */
void corgibl_limit_intensity(int limit)
{
	if (limit)
		corgibl_flags |= CORGIBL_BATTLOW;
	else
		corgibl_flags &= ~CORGIBL_BATTLOW;
	corgibl_send_intensity(corgi_backlight_device);
}
EXPORT_SYMBOL(corgibl_limit_intensity);


static struct backlight_properties corgibl_data = {
	.owner          = THIS_MODULE,
	.get_brightness = corgibl_get_intensity,
	.update_status  = corgibl_set_intensity,
};

static int __init corgibl_probe(struct platform_device *pdev)
{
	struct corgibl_machinfo *machinfo = pdev->dev.platform_data;

	bl_machinfo = machinfo;
	corgibl_data.max_brightness = machinfo->max_intensity;
	if (!machinfo->limit_mask)
		machinfo->limit_mask = -1;

	corgi_backlight_device = backlight_device_register ("corgi-bl",
		NULL, &corgibl_data);
	if (IS_ERR (corgi_backlight_device))
		return PTR_ERR (corgi_backlight_device);

	corgibl_data.power = FB_BLANK_UNBLANK;
	corgibl_data.brightness = machinfo->default_intensity;
	corgibl_send_intensity(corgi_backlight_device);

	printk("Corgi Backlight Driver Initialized.\n");
	return 0;
}

static int corgibl_remove(struct platform_device *dev)
{
	backlight_device_unregister(corgi_backlight_device);

	printk("Corgi Backlight Driver Unloaded\n");
	return 0;
}

static struct platform_driver corgibl_driver = {
	.probe		= corgibl_probe,
	.remove		= corgibl_remove,
	.suspend	= corgibl_suspend,
	.resume		= corgibl_resume,
	.driver		= {
		.name	= "corgi-bl",
	},
};

static int __init corgibl_init(void)
{
	return platform_driver_register(&corgibl_driver);
}

static void __exit corgibl_exit(void)
{
	platform_driver_unregister(&corgibl_driver);
}

module_init(corgibl_init);
module_exit(corgibl_exit);

MODULE_AUTHOR("Richard Purdie <rpurdie@rpsys.net>");
MODULE_DESCRIPTION("Corgi Backlight Driver");
MODULE_LICENSE("GPLv2");
