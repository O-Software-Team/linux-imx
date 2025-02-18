// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023, Ocompany
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#define DRV_NAME "visionox-rm69091"

struct visionox_rm69091 {
	struct drm_panel panel;
	struct regulator_bulk_data supplies[2];
	struct gpio_desc *reset_gpio;
	struct gpio_desc *elvddss_gpio;
	struct mipi_dsi_device *dsi;
	struct dentry *debugfs;
	u8 display_id[3];
	bool prepared;
	bool enabled;
	bool probed;
};

//
// struct panel_desc - Describes a simple panel.
//
struct panel_desc {
	/**
	 * @modes: Pointer to array of fixed modes appropriate for this panel.
	 *
	 * If only one mode then this can just be the address of the mode.
	 * NOTE: cannot be used with "timings" and also if this is specified
	 * then you cannot override the mode in the device tree.
	 */
	const struct drm_display_mode *modes;

	/** @num_modes: Number of elements in modes array. */
	unsigned int num_modes;

	/**
	 * @timings: Pointer to array of display timings
	 *
	 * NOTE: cannot be used with "modes" and also these will be used to
	 * validate a device tree override if one is present.
	 */
	const struct display_timing *timings;

	/** @num_timings: Number of elements in timings array. */
	unsigned int num_timings;

	/** @bpc: Bits per color. */
	unsigned int bpc;

	/** @size: Structure containing the physical size of this panel. */
	struct {
		/**
		 * @size.width: Width (in mm) of the active display area.
		 */
		unsigned int width;

		/**
		 * @size.height: Height (in mm) of the active display area.
		 */
		unsigned int height;
	} size;

	/** @delay: Structure containing various delay values for this panel. */
	struct {
		/**
		 * @delay.prepare: Time for the panel to become ready.
		 *
		 * The time (in milliseconds) that it takes for the panel to
		 * become ready and start receiving video data
		 */
		unsigned int prepare;

		/**
		 * @delay.enable: Time for the panel to display a valid frame.
		 *
		 * The time (in milliseconds) that it takes for the panel to
		 * display the first valid frame after starting to receive
		 * video data.
		 */
		unsigned int enable;

		/**
		 * @delay.disable: Time for the panel to turn the display off.
		 *
		 * The time (in milliseconds) that it takes for the panel to
		 * turn the display off (no content is visible).
		 */
		unsigned int disable;

		/**
		 * @delay.unprepare: Time to power down completely.
		 *
		 * The time (in milliseconds) that it takes for the panel
		 * to power itself down completely.
		 *
		 * This time is used to prevent a future "prepare" from
		 * starting until at least this many milliseconds has passed.
		 * If at prepare time less time has passed since unprepare
		 * finished, the driver waits for the remaining time.
		 */
		unsigned int unprepare;
	} delay;

	/** @bus_format: See MEDIA_BUS_FMT_... defines. */
	u32 bus_format;

	/** @bus_flags: See DRM_BUS_FLAG_... defines. */
	u32 bus_flags;

	/** @connector_type: LVDS, eDP, DSI, DPI, etc. */
	int connector_type;
};

static inline struct visionox_rm69091 *panel_to_ctx(struct drm_panel *panel)
{
	return container_of(panel, struct visionox_rm69091, panel);
}

static int visionox_rm69091_read_id(struct visionox_rm69091 *ctx)
{
        int ret;

        dev_info(ctx->panel.dev, "send NOOP\n");
	ret = mipi_dsi_dcs_nop(ctx->dsi);
	if (ret < 0) {
                dev_err(ctx->panel.dev, "could not send NOOP\n");
	}
        dev_info(ctx->panel.dev, "after NOOP\n");

        ret = mipi_dsi_dcs_read(ctx->dsi, MIPI_DCS_GET_DISPLAY_ID, ctx->display_id, 3);
        if (ret < 0) {
                dev_err(ctx->panel.dev, "could not read display ID\n");
                return ret;
        }
        dev_info(ctx->panel.dev, "MIPI display ID: manufacturer [%02x] version [%02x] driver [%02x]\n",
                ctx->display_id[0], ctx->display_id[1], ctx->display_id[2]);

        return 0;
}

static int visionox_rm69091_pwr_on_reset(struct visionox_rm69091 *ctx)
{
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0) {
		dev_err(ctx->panel.dev, "visionox_rm69091_pwr_on_reset bulk enable returned %d\n", ret);
		return ret;
	}

	/*
	 * Reset sequence of visionox panel requires the panel to be
	 * out of reset for 10ms, followed by being held in reset
	 * for 10ms and then out again
	 */

	dev_err(ctx->panel.dev, "visionox_rm69091_pwr_on_reset\n");
	gpiod_set_value(ctx->elvddss_gpio, 1);

	dev_err(ctx->panel.dev, "visionox_rm69091_pwr_on_reset \n");

	gpiod_set_value(ctx->reset_gpio, 0);
	usleep_range(5000, 5000);

	dev_err(ctx->panel.dev, "visionox_rm69091_pwr_on_reset RESET HIGH\n"); /* Asserted means set LOW */

	gpiod_set_value(ctx->reset_gpio, 1);
	usleep_range(15000, 15000);

	dev_err(ctx->panel.dev, "visionox_rm69091_pwr_on_reset RESET LOW\n");

	gpiod_set_value(ctx->reset_gpio, 0);
	usleep_range(15000, 15000);

	dev_err(ctx->panel.dev, "visionox_rm69091_pwr_on_reset RESET HIGH\n");

	dev_err(ctx->panel.dev, "visionox_rm69091_pwr_on_reset finish\n");
	return 0;
}

static int visionox_rm69091_pwr_off(struct visionox_rm69091 *ctx)
{
	gpiod_set_value(ctx->reset_gpio, 0);
	gpiod_set_value(ctx->elvddss_gpio, 0);

	dev_err(ctx->panel.dev, "visionox_rm69091_pwr_off currently\n");

	return regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
}

static int visionox_rm69091_unprepare(struct drm_panel *panel)
{
	struct visionox_rm69091 *ctx = panel_to_ctx(panel);
	int ret;

	if (ctx->prepared == false)
		return 0;
	dev_err(ctx->panel.dev, "visionox_rm69091_unprepare\n");

	ctx->dsi->mode_flags = 0;

	ret = visionox_rm69091_pwr_off(ctx);
	if (ret < 0) {
		dev_err(ctx->panel.dev, "pwr_off failed, ret = %d\n", ret);
	}

	ctx->prepared = false;
	dev_err(ctx->panel.dev, "visionox_rm69091_unprepare finish\n");
	return ret;
}


static void visionox_rm69091_init_sequence(struct visionox_rm69091 *ctx)
{
	int ret;

	ret = mipi_dsi_dcs_write(ctx->dsi, 0xfe, (u8[]) { 0x00 }, 1);
	if (ret < 0) {
		dev_err(ctx->panel.dev, "cmd set tx step 1 failed, ret = %d\n", ret);
	}

	ret = mipi_dsi_dcs_write(ctx->dsi, 0x31, (u8[]) { 0x00, 0x28, 0x01, 0xb9 }, 4);
	if (ret < 0) {
		dev_err(ctx->panel.dev, "cmd set tx step 2 failed, ret = %d\n", ret);
	}

	ret = mipi_dsi_dcs_write(ctx->dsi, 0x30, (u8[]) { 0x00, 0x01, 0x01, 0xda }, 4);
	if (ret < 0) {
		dev_err(ctx->panel.dev, "cmd set tx step 3 failed, ret = %d\n", ret);
	}

	ret = mipi_dsi_dcs_write(ctx->dsi, 0x12, (u8[]) { 0x00 }, 1);
	if (ret < 0) {
		dev_err(ctx->panel.dev, "cmd set tx step 4 failed, ret = %d\n", ret);
	}

	ret = mipi_dsi_dcs_write(ctx->dsi, 0x2a, (u8[]) { 0x00, 0x28, 0x01, 0xb9 }, 4);
	if (ret < 0) {
		dev_err(ctx->panel.dev, "cmd set tx step 5 failed, ret = %d\n", ret);
	}

	ret = mipi_dsi_dcs_write(ctx->dsi, 0x2b, (u8[]) { 0x00, 0x00, 0x01, 0xdb }, 4);
	if (ret < 0) {
		dev_err(ctx->panel.dev, "cmd set tx step 6 failed, ret = %d\n", ret);
	}

	ret = mipi_dsi_dcs_write(ctx->dsi, 0x35, (u8[]) { 0x00 }, 1);
	if (ret < 0) {
		dev_err(ctx->panel.dev, "cmd set tx step 7 failed, ret = %d\n", ret);
	}

	ret = mipi_dsi_dcs_write(ctx->dsi, 0x51, (u8[]) { 0xff }, 1);
	if (ret < 0) {
		dev_err(ctx->panel.dev, "cmd set tx step 8 failed, ret = %d\n", ret);
	}
}

static int visionox_rm69091_enable(struct drm_panel *panel)
{
	struct visionox_rm69091 *ctx = panel_to_ctx(panel);
	int ret;

	if (ctx->enabled == true)
		return 0;

	dev_err(ctx->panel.dev, "visionox_rm69091_enable\n");

	ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	visionox_rm69091_init_sequence(ctx);

	ctx->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_write(ctx->dsi, 0x11, (u8[]) { 0x0 }, 0);
	if (ret < 0) {
		dev_err(ctx->panel.dev, "exit_sleep_mode cmd failed ret = %d\n", ret);
		return ret;
	}

	/* Per DSI spec wait 120ms after sending exit sleep DCS command */
	msleep(120);

	ret = mipi_dsi_dcs_write(ctx->dsi, 0x29, (u8[]) { 0x00 }, 0);
	if (ret < 0) {
		dev_err(ctx->panel.dev, "set_display_on cmd failed ret = %d\n", ret);
		return ret;
	}
	msleep(20);

	ctx->enabled = true;
	return 0;
}

static int visionox_rm69091_disable(struct drm_panel *panel)
{
	struct visionox_rm69091 *ctx = panel_to_ctx(panel);
	int ret;

        if (!ctx->enabled)
                return 0;

	ret = mipi_dsi_dcs_write(ctx->dsi, MIPI_DCS_SET_DISPLAY_OFF, NULL, 0);
	if (ret < 0)
		dev_err(ctx->panel.dev, "set_display_off cmd failed ret = %d\n", ret);

	/* 120ms delay required here as per DCS spec */
	msleep(120);

	ret = mipi_dsi_dcs_write(ctx->dsi, MIPI_DCS_ENTER_SLEEP_MODE, NULL, 0);
	if (ret < 0) {
		dev_err(ctx->panel.dev, "enter_sleep cmd failed ret = %d\n", ret);
	}

	ctx->enabled = false;
	return 0;
}

static int visionox_rm69091_prepare(struct drm_panel *panel)
{
	struct visionox_rm69091 *ctx = panel_to_ctx(panel);
	int ret;

	dev_err(ctx->panel.dev, "visionox_rm69091_prepare\n");

	ret = visionox_rm69091_pwr_on_reset(ctx);
	if (ret < 0)
		return ret;

	dev_err(ctx->panel.dev, "visionox_rm69091 POWER_ON in prepare\n");
   
	ctx->prepared = true;

	return 0;
}

struct panel_desc_dsi {
	struct panel_desc desc;

	unsigned long flags;
	enum mipi_dsi_pixel_format format;
	unsigned int lanes;
};

static const struct drm_display_mode visionox_rm69091_402x476_60hz_mode = {
	.name        = "OPPO 402x476",
	.clock       = (402 + 10 + 10 + 2) * (476 + 10 + 2 + 10) * 59.7 / 1000, /* 12605 */
	.hdisplay    = 402,
	.hsync_start = 402 + 10,
	.hsync_end   = 402 + 10 + 2,
	.htotal      = 402 + 10 + 2 + 10,

	.vdisplay    = 476,
	.vsync_start = 476 + 10,
	.vsync_end   = 476 + 10 + 2,
	.vtotal      = 476 + 10 + 2 + 10,
	.width_mm    = 31,
	.height_mm   = 37,
};

static const struct panel_desc_dsi visionox_rm69091_402x476_60hz = {
	.desc = {
		.modes = &visionox_rm69091_402x476_60hz_mode,
		.num_modes = 1,
		.bpc = 24,
		.size = {
			.width = 402,
			.height = 476,
		},
		.connector_type = DRM_MODE_CONNECTOR_DSI,
	},
	.flags = MIPI_DSI_MODE_VIDEO |
		 MIPI_DSI_CLOCK_NON_CONTINUOUS |
		 MIPI_DSI_MODE_LPM |
		 DRM_MODE_FLAG_NHSYNC |
		 DRM_MODE_FLAG_NVSYNC,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 1,
};

static const struct of_device_id dsi_of_match[] = {
	{
		.compatible = "visionox,rm69091",
		.data = &visionox_rm69091_402x476_60hz,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, dsi_of_match);

//
// Panel drm functions
//
static int visionox_rm69091_get_modes(struct drm_panel *panel,
				      struct drm_connector *connector)
{
	struct visionox_rm69091 *ctx = panel_to_ctx(panel);
	struct drm_display_mode *mode;

	dev_err(ctx->panel.dev, "visionox_rm69091_get_modes\n");
	mode = drm_mode_duplicate(connector->dev,
			  &visionox_rm69091_402x476_60hz_mode);
	if (!mode) {
		dev_err(ctx->panel.dev, "failed to create a new display mode\n");
		return 0;
	}

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	dev_err(ctx->panel.dev, "visionox_rm69091_get_modes finish\n");
	return 1;
}

static const struct drm_panel_funcs visionox_rm69091_drm_funcs = {
	.unprepare = visionox_rm69091_unprepare,
	.prepare = visionox_rm69091_prepare,
	.enable = visionox_rm69091_enable,
	.disable = visionox_rm69091_disable,
	.get_modes = visionox_rm69091_get_modes,
};

//
// PIXEL TEST
//
static int allpixelson_set(void *data, u64 val)
{
	struct visionox_rm69091 *ctx = data;

	msleep(val * 1000);

	/* Reset the panel to get video back */

	visionox_rm69091_prepare(&ctx->panel);

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(allpixelson_fops, NULL,
			allpixelson_set, "%llu\n");

static void visionox_rm69091_debugfs_init(struct visionox_rm69091 *ctx)
{
	ctx->debugfs = debugfs_create_dir(DRV_NAME, NULL);

	debugfs_create_file("allpixelson", 0600, ctx->debugfs, ctx,
			    &allpixelson_fops);
}

static void visionox_rm69091_debugfs_remove(struct visionox_rm69091 *ctx)
{
	debugfs_remove_recursive(ctx->debugfs);
	ctx->debugfs = NULL;
}

//
// dsi functions
//
static int visionox_rm69091_dsi_probe(struct mipi_dsi_device *dsi)
{
	const struct panel_desc_dsi *desc;
	const struct of_device_id *id;
	struct device *dev = &dsi->dev;
	struct visionox_rm69091 *ctx;
	int ret;

	dev_info(dev, "visionox_rm69091_probe [device = %s]\n", dsi->name);

	id = of_match_node(dsi_of_match, dsi->dev.of_node);
	if (!id) {
		dev_err(dev, "dsi_probe did'nt find dsi_of_match [device = %s]\n",
			dsi->name);
		return -ENODEV;
	}

	desc = id->data;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->panel.dev = dev;
	ctx->dsi = dsi;

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->supplies[0].supply = "buck7-supply"; /* VDD_1V8 */
	ctx->supplies[0].init_load_uA = 55000;
	ctx->supplies[1].supply = "buck6-supply"; /* VDD_3V3 */
	ctx->supplies[1].init_load_uA = 32000;

	ret = devm_regulator_bulk_get(ctx->panel.dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret < 0) {
		dev_err(dev, "Can't get bulk regulator %s (%d)\n", ctx->supplies[0].supply, ret);
		return ret;
	}

	ctx->reset_gpio = devm_gpiod_get(ctx->panel.dev,
					 "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(dev, "cannot get reset gpio %ld\n", PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}
	ctx->elvddss_gpio = devm_gpiod_get(ctx->panel.dev,
					 "elvddss", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->elvddss_gpio)) {
		dev_err(dev, "cannot get elvddss gpio %ld\n", PTR_ERR(ctx->elvddss_gpio));
		return PTR_ERR(ctx->elvddss_gpio);
	}
	drm_panel_init(&ctx->panel, dev, &visionox_rm69091_drm_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	ctx->panel.dev = dev;
	ctx->panel.funcs = &visionox_rm69091_drm_funcs;
	drm_panel_add(&ctx->panel);

	dsi->mode_flags = desc->flags;
	dsi->format = desc->format;
	dsi->lanes = desc->lanes;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "dsi attach failed ret = %d\n", ret);
		goto err_dsi_attach;
	}

	//dev_set_drvdata(dev, &ctx->panel);

	visionox_rm69091_debugfs_init(ctx);

	dev_info(dev, "visionox_rm69091_probe finish\n");
	return 0;

err_set_load:
	dev_err(dev, "visionox_rm69091_probe err_set_load\n");
	mipi_dsi_detach(dsi);
err_dsi_attach:
	dev_err(dev, "visionox_rm69091_probe err_dsi_attach\n");
	drm_panel_remove(&ctx->panel);
	return ret;
}

static void visionox_rm69091_dsi_remove(struct mipi_dsi_device *dsi)
{
	struct visionox_rm69091 *ctx = mipi_dsi_get_drvdata(dsi);

	dev_err(ctx->panel.dev, "visionox_rm69091_remove\n");
	mipi_dsi_detach(ctx->dsi);

	visionox_rm69091_debugfs_remove(ctx);

	drm_panel_remove(&ctx->panel);
}

static void visionox_rm69091_dsi_shutdown(struct mipi_dsi_device *dsi)
{
	struct visionox_rm69091 *ctx = mipi_dsi_get_drvdata(dsi);

        drm_panel_unprepare(&ctx->panel);
}

static struct mipi_dsi_driver visionox_rm69091_dsi_driver = {
	.driver = {
		.name = "visionox-rm69091-dsi",
		.of_match_table = dsi_of_match,
	},
	.probe = visionox_rm69091_dsi_probe,
	.remove = visionox_rm69091_dsi_remove,
	.shutdown = visionox_rm69091_dsi_shutdown,
};

//
// platform entry points
//
static int __init visionox_rm69091_init(void)
{
	int err;

	err = mipi_dsi_driver_register(&visionox_rm69091_dsi_driver);
	return err;
}
module_init(visionox_rm69091_init);

static void __exit visionox_rm69091_exit(void)
{
	mipi_dsi_driver_unregister(&visionox_rm69091_dsi_driver);
}
module_exit(visionox_rm69091_exit);

MODULE_SOFTDEP("pre: rohm_bd718x7, nwl-dsi");
MODULE_AUTHOR("Daniel Fields<dfields@osoftware.com>");
MODULE_DESCRIPTION("Visionox RM69091 OPPO display MIPI DSI Driver");
MODULE_LICENSE("GPL v2");
