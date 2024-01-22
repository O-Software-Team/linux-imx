// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023, Ocompany
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct visionox_rm69091 {
	struct drm_panel panel;
//	struct regulator_bulk_data supplies[1];
	struct gpio_desc *reset_gpio;
	struct mipi_dsi_device *dsi;
	bool prepared;
	bool enabled;
};

static inline struct visionox_rm69091 *panel_to_ctx(struct drm_panel *panel)
{
	return container_of(panel, struct visionox_rm69091, panel);
}

static int visionox_rm69091_power_on(struct visionox_rm69091 *ctx)
{
	int ret;

//	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
//	if (ret < 0)
//		return ret;

	/*
	 * Reset sequence of visionox panel requires the panel to be
	 * out of reset for 10ms, followed by being held in reset
	 * for 10ms and then out again
	 */
	//dev_err(ctx->panel.dev, "visionox_rm69091_power_on \n");

	gpiod_set_value(ctx->reset_gpio, 1);
	usleep_range(10000, 20000);
	gpiod_set_value(ctx->reset_gpio, 0);
	usleep_range(10000, 20000);
	gpiod_set_value(ctx->reset_gpio, 1);
	usleep_range(10000, 20000);

	//dev_err(ctx->panel.dev, "visionox_rm69091_power_on finish\n");
	return 0;
}

static int visionox_rm69091_power_off(struct visionox_rm69091 *ctx)
{
	gpiod_set_value(ctx->reset_gpio, 0);

	return (0);
//	return regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
}

static int visionox_rm69091_unprepare(struct drm_panel *panel)
{
	struct visionox_rm69091 *ctx = panel_to_ctx(panel);
	int ret;

	ctx->dsi->mode_flags = 0;

	dev_err(ctx->panel.dev, "visionox_rm69091_unprepare\n");

	ret = mipi_dsi_dcs_write(ctx->dsi, MIPI_DCS_SET_DISPLAY_OFF, NULL, 0);
	if (ret < 0)
		dev_err(ctx->panel.dev, "set_display_off cmd failed ret = %d\n", ret);

	/* 120ms delay required here as per DCS spec */
	msleep(120);

	ret = mipi_dsi_dcs_write(ctx->dsi, MIPI_DCS_ENTER_SLEEP_MODE, NULL, 0);
	if (ret < 0) {
		dev_err(ctx->panel.dev, "enter_sleep cmd failed ret = %d\n", ret);
	}

	ret = visionox_rm69091_power_off(ctx);

	ctx->prepared = false;
	dev_err(ctx->panel.dev, "visionox_rm69091_unprepare finish\n");
	return ret;
}

static int visionox_rm69091_prepare(struct drm_panel *panel)
{
	struct visionox_rm69091 *ctx = panel_to_ctx(panel);
	int ret;

	dev_err(ctx->panel.dev, "visionox_rm69091_prepare\n");

	if (ctx->prepared)
		return 0;

	ret = visionox_rm69091_power_on(ctx);
	if (ret < 0)
		return ret;

	ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_write_buffer(ctx->dsi, (u8[]) { 0xfe, 0x00 }, 2);
	if (ret < 0) {
		dev_err(ctx->panel.dev, "cmd set tx 0 failed, ret = %d\n", ret);
		goto power_off;
	}

	ret = mipi_dsi_dcs_write_buffer(ctx->dsi, (u8[]) { 0xc2, 0x08 }, 2);
	if (ret < 0) {
		dev_err(ctx->panel.dev, "cmd set tx 1 failed, ret = %d\n", ret);
		goto power_off;
	}

	ret = mipi_dsi_dcs_write_buffer(ctx->dsi, (u8[]) { 0x35, 0x00 }, 2);
	if (ret < 0) {
		dev_err(ctx->panel.dev, "cmd set tx 2 failed, ret = %d\n", ret);
		goto power_off;
	}

	ret = mipi_dsi_dcs_write_buffer(ctx->dsi, (u8[]) { 0x51, 0xff }, 2);
	if (ret < 0) {
		dev_err(ctx->panel.dev, "cmd set tx 3 failed, ret = %d\n", ret);
		goto power_off;
	}

	ret = mipi_dsi_dcs_write(ctx->dsi, MIPI_DCS_EXIT_SLEEP_MODE, NULL, 0);
	if (ret < 0) {
	dev_err(ctx->panel.dev, "exit_sleep_mode cmd failed ret = %d\n", ret);
		goto power_off;
	}

	/* Per DSI spec wait 120ms after sending exit sleep DCS command */
	msleep(120);

	ret = mipi_dsi_dcs_write(ctx->dsi, MIPI_DCS_SET_DISPLAY_ON, NULL, 0);
	if (ret < 0) {
		dev_err(ctx->panel.dev, "set_display_on cmd failed ret = %d\n", ret);
		goto power_off;
	}

	/* Per DSI spec wait 120ms after sending set_display_on DCS command */
	msleep(120);

	ctx->prepared = true;
   
	dev_err(ctx->panel.dev, "visionox_rm69091_prepare finish\n");
	return 0;

power_off:
	dev_err(ctx->panel.dev, "visionox_rm69091_prepare poweroff\n");
	return ret;
}

static const struct drm_display_mode visionox_rm69091_480x480_60hz = {
	.name = "1080x2248",
	.clock = 158695,
	.hdisplay = 1080,
	.hsync_start = 1080 + 26,
	.hsync_end = 1080 + 26 + 2,
	.htotal = 1080 + 26 + 2 + 36,
	.vdisplay = 2248,
	.vsync_start = 2248 + 56,
	.vsync_end = 2248 + 56 + 4,
	.vtotal = 2248 + 56 + 4 + 4,
	.flags = 0,
};

static int visionox_rm69091_get_modes(struct drm_panel *panel,
				      struct drm_connector *connector)
{
	struct visionox_rm69091 *ctx = panel_to_ctx(panel);
	struct drm_display_mode *mode;

	dev_err(ctx->panel.dev, "visionox_rm69091_get_modes\n");
	mode = drm_mode_duplicate(connector->dev,
				  &visionox_rm69091_480x480_60hz);
	if (!mode) {
		dev_err(ctx->panel.dev, "failed to create a new display mode\n");
		return 0;
	}

	connector->display_info.width_mm = 30;
	connector->display_info.height_mm = 40;
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	dev_err(ctx->panel.dev, "visionox_rm69091_get_modes finish\n");
	return 1;
}

static const struct drm_panel_funcs visionox_rm69091_drm_funcs = {
	.unprepare = visionox_rm69091_unprepare,
	.prepare = visionox_rm69091_prepare,
	.get_modes = visionox_rm69091_get_modes,
};

static int visionox_rm69091_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct visionox_rm69091 *ctx;
	int ret;

	dev_err(dev, "visionox_rm69091_probe\n");

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->panel.dev = dev;
	ctx->dsi = dsi;

//	ctx->supplies[0].supply = "regulator-vsd-3v3";

//	ret = devm_regulator_bulk_get(ctx->panel.dev, ARRAY_SIZE(ctx->supplies),
//				      ctx->supplies);
//	if (ret < 0) {
//		dev_err(dev, "Can't get bulk regulator %s (%d)\n", ctx->supplies[0].supply, ret);
//		return ret;
//	}

	ctx->reset_gpio = devm_gpiod_get(ctx->panel.dev,
					 "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(dev, "cannot get reset gpio %ld\n", PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}

	drm_panel_init(&ctx->panel, dev, &visionox_rm69091_drm_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	ctx->panel.dev = dev;
	ctx->panel.funcs = &visionox_rm69091_drm_funcs;
	drm_panel_add(&ctx->panel);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_LPM |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS;
	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "dsi attach failed ret = %d\n", ret);
		goto err_dsi_attach;
	}

//	ret = regulator_set_load(ctx->supplies[0].consumer, 32000);
//	if (ret) {
//		dev_err(dev, "regulator set load failed for vdda supply ret = %d\n", ret);
//		goto err_set_load;
//	}

	dev_err(dev, "visionox_rm69091_probe finish\n");
	return 0;

err_set_load:
	mipi_dsi_detach(dsi);
err_dsi_attach:
	drm_panel_remove(&ctx->panel);
	return ret;
}

static void visionox_rm69091_remove(struct mipi_dsi_device *dsi)
{
	struct visionox_rm69091 *ctx = mipi_dsi_get_drvdata(dsi);

	dev_err(ctx->panel.dev, "visionox_rm69091_remove\n");
	mipi_dsi_detach(ctx->dsi);
	mipi_dsi_device_unregister(ctx->dsi);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id visionox_rm69091_of_match[] = {
	{ .compatible = "raydium,rm69091" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, visionox_rm69091_of_match);

static struct mipi_dsi_driver visionox_rm69091_driver = {
	.driver = {
		.name = "panel-visionox-rm69091",
		.of_match_table = visionox_rm69091_of_match,
	},
	.probe = visionox_rm69091_probe,
	.remove = visionox_rm69091_remove,
};
module_mipi_dsi_driver(visionox_rm69091_driver);

MODULE_DESCRIPTION("Visionox RM69091 DSI Panel Driver");
MODULE_LICENSE("GPL v2");
