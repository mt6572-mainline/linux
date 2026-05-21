// SPDX-License-Identifier: GPL-2.0

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct otm8018b {
	struct device *dev;
	struct drm_panel panel;
	struct gpio_desc *reset_gpio;
	struct regulator *vcc;
	const struct panel_desc *desc;
	bool prepared;
};

struct panel_desc {
	const struct drm_display_mode *display_mode;
	unsigned long mode_flags;
	void (*init_sequence)(struct mipi_dsi_multi_context *ctx);
};

static const struct drm_display_mode otm8018b_boyi_mode = {
	.clock = (480 + 100 + 10 + 100) * (854 + 20 + 6 + 20) * 60 / 1000,
	.hdisplay = 480,
	.hsync_start = 480 + 100,
	.hsync_end = 480 + 100 + 10,
	.htotal = 480 + 100 + 10 + 100,
	.vdisplay = 854,
	.vsync_start = 854 + 20,
	.vsync_end = 854 + 20 + 6,
	.vtotal = 854 + 20 + 6 + 20,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
	.width_mm = 56,
	.height_mm = 100,
};

static const struct drm_display_mode otm8018b_djn_mode = {
	.clock = 31134,
	.hdisplay = 480,
	.hsync_start = 480 + 33,
	.hsync_end = 480 + 33 + 6,
	.htotal = 480 + 33 + 6 + 33,
	.vdisplay = 800,
	.vsync_start = 800 + 28,
	.vsync_end = 800 + 28 + 84,
	.vtotal = 800 + 28 + 84 + 28,
	.width_mm = 53,
	.height_mm = 88,
};

static inline struct otm8018b *panel_to_otm8018b(struct drm_panel *panel)
{
	return container_of(panel, struct otm8018b, panel);
}

static int otm8018b_prepare(struct drm_panel *panel)
{
	struct otm8018b *ctx = panel_to_otm8018b(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };
	int ret;

	ret = regulator_enable(ctx->vcc);
	if (ret < 0) {
		dev_err(panel->dev, "failed to enable vcc: %d\n", ret);
		return ret;
	}
	msleep(20);

	if (ctx->reset_gpio) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 0);
		usleep_range(1000, 2000);

		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		usleep_range(10000, 11000);

		gpiod_set_value_cansleep(ctx->reset_gpio, 0);
		msleep(50);
	}

	if (ctx->desc->init_sequence)
		ctx->desc->init_sequence(&dsi_ctx);

	if (dsi_ctx.accum_err) {
		regulator_disable(ctx->vcc);
		return dsi_ctx.accum_err;
	}

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 150);

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 50);

	ctx->prepared = true;
	return 0;
}

static int otm8018b_unprepare(struct drm_panel *panel)
{
	struct otm8018b *ctx = panel_to_otm8018b(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	if (ctx->reset_gpio)
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);

	regulator_disable(ctx->vcc);
	ctx->prepared = false;
	return dsi_ctx.accum_err;
}

static int otm8018b_get_modes(struct drm_panel *panel,
			      struct drm_connector *connector)
{
	struct otm8018b *ctx = panel_to_otm8018b(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, ctx->desc->display_mode);
	if (!mode)
		return -ENOMEM;

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;

	return 1;
}

static const struct drm_panel_funcs otm8018b_drm_funcs = {
	.prepare = otm8018b_prepare,
	.unprepare = otm8018b_unprepare,
	.get_modes = otm8018b_get_modes,
};

static int otm8018b_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct otm8018b *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct otm8018b, panel,
				   &otm8018b_drm_funcs, DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "failed to get reset-gpios\n");

	ctx->vcc = devm_regulator_get(dev, "vcc");
	if (IS_ERR(ctx->vcc))
		return dev_err_probe(dev, PTR_ERR(ctx->vcc),
				     "failed to request vcc supply\n");

	ctx->desc = device_get_match_data(dev);

	mipi_dsi_set_drvdata(dsi, ctx);
	ctx->dev = dev;

	dsi->lanes = 2;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = ctx->desc->mode_flags;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret) {
		dev_err(dev, "mipi_dsi_attach failed: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void otm8018b_remove(struct mipi_dsi_device *dsi)
{
	struct otm8018b *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static void otm8018b_djn_init(struct mipi_dsi_multi_context *ctx)
{
	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x00);

	mipi_dsi_generic_write_seq_multi(ctx, 0xFF, 0x80, 0x09, 0x01);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x80);
	mipi_dsi_generic_write_seq_multi(ctx, 0xFF, 0x80, 0x09);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x80);

	mipi_dsi_generic_write_seq_multi(ctx, 0xF5, 0x01, 0x18, 0x02, 0x18, 0x10,
					 0x18, 0x02, 0x18, 0x0e, 0x18, 0x0f, 0x20);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x90);
	mipi_dsi_generic_write_seq_multi(ctx, 0xF5, 0x02, 0x18, 0x08, 0x18, 0x06,
					 0x18, 0x0d, 0x18, 0x0b, 0x18);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0xA0);
	mipi_dsi_generic_write_seq_multi(ctx, 0xF5, 0x10, 0x18, 0x01, 0x18, 0x14,
					 0x18, 0x14, 0x18);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0xB0);
	mipi_dsi_generic_write_seq_multi(ctx, 0xF5, 0x14, 0x18, 0x12, 0x18, 0x13,
					 0x18, 0x11, 0x18, 0x13, 0x18, 0x00, 0x00);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x8B);
	mipi_dsi_generic_write_seq_multi(ctx, 0xB0, 0x40);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0xC0);
	mipi_dsi_generic_write_seq_multi(ctx, 0xC5, 0x00);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x00);
	mipi_dsi_generic_write_seq_multi(ctx, 0xD8, 0x43, 0x43);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0xB1);
	mipi_dsi_generic_write_seq_multi(ctx, 0xC5, 0xA9);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x90);
	mipi_dsi_generic_write_seq_multi(ctx, 0xC5, 0x96, 0xA7, 0x01);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x82);
	mipi_dsi_generic_write_seq_multi(ctx, 0xC5, 0xA3);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x81);
	mipi_dsi_generic_write_seq_multi(ctx, 0xC1, 0x66);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0xA0);
	mipi_dsi_generic_write_seq_multi(ctx, 0xC1, 0xEA);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0xA1);
	mipi_dsi_generic_write_seq_multi(ctx, 0xC1, 0x08);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0xA2);
	mipi_dsi_generic_write_seq_multi(ctx, 0xC0, 0x02, 0x1B);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x80);
	mipi_dsi_generic_write_seq_multi(ctx, 0xC4, 0x30);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x81);
	mipi_dsi_generic_write_seq_multi(ctx, 0xC4, 0x83);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x88);
	mipi_dsi_generic_write_seq_multi(ctx, 0xC4, 0x80);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0xA1);
	mipi_dsi_generic_write_seq_multi(ctx, 0xB3, 0x10);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0xB4);
	mipi_dsi_generic_write_seq_multi(ctx, 0xC0, 0x50);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x36, 0x00);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x90);
	mipi_dsi_generic_write_seq_multi(ctx, 0xC0, 0x00, 0x44, 0x00, 0x00, 0x00, 0x03);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0xA6);
	mipi_dsi_generic_write_seq_multi(ctx, 0xC1, 0x01, 0x00, 0x00);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x80);
	mipi_dsi_generic_write_seq_multi(ctx, 0xCE, 0x87, 0x03, 0x14, 0x86, 0x03, 0x14);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x90);
	mipi_dsi_generic_write_seq_multi(ctx, 0xCE, 0x33, 0x1E, 0x14, 0x33, 0x1F, 0x14);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0xA0);
	mipi_dsi_generic_write_seq_multi(ctx, 0xCE, 0x38, 0x03, 0x03, 0x1C, 0x00,
					 0x14, 0x00, 0x38, 0x02, 0x03, 0x1D, 0x00,
					 0x14, 0x00);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0xB0);
	mipi_dsi_generic_write_seq_multi(ctx, 0xCE, 0x38, 0x01, 0x03, 0x1E, 0x00,
					 0x14, 0x00, 0x38, 0x00, 0x03, 0x1F, 0x00,
					 0x14, 0x00);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0xC0);
	mipi_dsi_generic_write_seq_multi(ctx, 0xCE, 0x30, 0x00, 0x03, 0x20, 0x00,
					 0x14, 0x00, 0x30, 0x01, 0x03, 0x21, 0x00,
					 0x14, 0x00);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0xD0);
	mipi_dsi_generic_write_seq_multi(ctx, 0xCE, 0x30, 0x02, 0x03, 0x22, 0x00,
					 0x14, 0x00, 0x30, 0x03, 0x03, 0x23, 0x00,
					 0x14, 0x00);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0xC6);
	mipi_dsi_generic_write_seq_multi(ctx, 0xCF, 0x01, 0x80);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0xC9);
	mipi_dsi_generic_write_seq_multi(ctx, 0xCF, 0x10);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0xC0);
	mipi_dsi_generic_write_seq_multi(ctx, 0xCB, 0x00, 0x54, 0x54, 0x54, 0x54,
					 0x00, 0x00, 0x54, 0x54, 0x54, 0x54, 0x00,
					 0x00, 0x00, 0x00);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0xD0);
	mipi_dsi_generic_write_seq_multi(ctx, 0xCB, 0x00, 0x00, 0x00, 0x00, 0x00,
					 0x00, 0x54, 0x54, 0x54, 0x54, 0x00, 0x00,
					 0x54, 0x54, 0x54);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0xE0);
	mipi_dsi_generic_write_seq_multi(ctx, 0xCB, 0x54, 0x00, 0x00, 0x00, 0x00,
					 0x00, 0x00, 0x00, 0x00, 0x00);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x80);
	mipi_dsi_generic_write_seq_multi(ctx, 0xCC, 0x00, 0x26, 0x25, 0x02, 0x06,
					 0x00, 0x00, 0x0A, 0x0E, 0x0C);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x90);
	mipi_dsi_generic_write_seq_multi(ctx, 0xCC, 0x10, 0x00, 0x00, 0x00, 0x00,
					 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x26,
					 0x25, 0x01, 0x05);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0xA0);
	mipi_dsi_generic_write_seq_multi(ctx, 0xCC, 0x00, 0x00, 0x09, 0x0D, 0x0B,
					 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
					 0x00, 0x00, 0x00);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0xB0);
	mipi_dsi_generic_write_seq_multi(ctx, 0xCC, 0x00, 0x25, 0x26, 0x05, 0x01,
					 0x00, 0x00, 0x0F, 0x0B, 0x0D);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0xC0);
	mipi_dsi_generic_write_seq_multi(ctx, 0xCC, 0x09, 0x00, 0x00, 0x00, 0x00,
					 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x25,
					 0x26, 0x06, 0x02);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0xD0);
	mipi_dsi_generic_write_seq_multi(ctx, 0xCC, 0x00, 0x00, 0x10, 0x0C, 0x0E,
					 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
					 0x00, 0x00, 0x00);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x00);
	mipi_dsi_generic_write_seq_multi(ctx, 0xD9, 0x31);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x00);
	mipi_dsi_generic_write_seq_multi(ctx, 0xE1, 0x06, 0x07, 0x0E, 0x0D, 0x07,
					 0x16, 0x0C, 0x0C, 0x02, 0x06, 0x05, 0x07,
					 0x0F, 0x2B, 0x27, 0x0D);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x00);
	mipi_dsi_generic_write_seq_multi(ctx, 0xE2, 0x06, 0x07, 0x0E, 0x0D, 0x07,
					 0x16, 0x0C, 0x0C, 0x02, 0x06, 0x05, 0x07,
					 0x0F, 0x2B, 0x27, 0x0D);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x00);
	mipi_dsi_generic_write_seq_multi(ctx, 0xFF, 0xFF, 0xFF, 0xFF);

	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x3A, 0x77);
}


static const struct panel_desc otm8018b_boyi_desc = {
	.display_mode = &otm8018b_boyi_mode,
	.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
		      MIPI_DSI_MODE_LPM | MIPI_DSI_CLOCK_NON_CONTINUOUS
};

static const struct panel_desc otm8018b_djn_desc = {
	.display_mode = &otm8018b_djn_mode,
	.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
		      MIPI_DSI_MODE_LPM,
	.init_sequence = otm8018b_djn_init
};

static const struct of_device_id orisetech_otm8018b_of_match[] = {
	{ .compatible = "boyi,otm8018b", .data = &otm8018b_boyi_desc },
	{ .compatible = "djn,otm8018b", .data = &otm8018b_djn_desc },
	{}
};
MODULE_DEVICE_TABLE(of, orisetech_otm8018b_of_match);

static struct mipi_dsi_driver orisetech_otm8018b_driver = {
	.probe  = otm8018b_probe,
	.remove = otm8018b_remove,
	.driver = {
		.name = "panel-orisetech-otm8018b",
		.of_match_table = orisetech_otm8018b_of_match,
	},
};
module_mipi_dsi_driver(orisetech_otm8018b_driver);

MODULE_DESCRIPTION("DRM driver for OriseTech OTM8018B MIPI-DSI panel");
MODULE_LICENSE("GPL v2");
