// SPDX-License-Identifier: GPL-2.0-only
/*
 * GMA500 Backlight Interface
 *
 * Copyright (c) 2009-2011, Intel Corporation.
 *
 * Authors: Eric Knopp
 */

#include <linux/backlight.h>
#include <acpi/video.h>

#include <drm/drm_print.h>

#include "psb_drv.h"
#include "psb_intel_reg.h"
#include "psb_intel_drv.h"
#include "intel_bios.h"
#include "power.h"

void gma_backlight_enable(struct drm_device *dev)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);

	dev_priv->backlight_enabled = true;
	dev_priv->ops->backlight_set(dev, dev_priv->backlight_level);
}

void gma_backlight_disable(struct drm_device *dev)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);

	dev_priv->backlight_enabled = false;
	dev_priv->ops->backlight_set(dev, 0);
}

void gma_backlight_set(struct drm_device *dev, int v)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);

	dev_priv->backlight_level = v;
	if (dev_priv->backlight_enabled)
		dev_priv->ops->backlight_set(dev, v);
}

static int gma_backlight_get_brightness(struct backlight_device *bd)
{
	struct drm_device *dev = bl_get_data(bd);
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);

	if (dev_priv->ops->backlight_get)
		return dev_priv->ops->backlight_get(dev);

	return dev_priv->backlight_level;
}

static int gma_backlight_update_status(struct backlight_device *bd)
{
	struct drm_device *dev = bl_get_data(bd);
	int level = backlight_get_brightness(bd);

	/* Percentage 1-100% being valid */
	if (level < 1)
		level = 1;

	gma_backlight_set(dev, level);
	return 0;
}

static const struct backlight_ops gma_backlight_ops __maybe_unused = {
	.get_brightness = gma_backlight_get_brightness,
	.update_status  = gma_backlight_update_status,
};

int gma_backlight_init(struct drm_device *dev, struct drm_connector *connector)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);
	int ret;

	dev_priv->backlight_enabled = true;
	dev_priv->backlight_level = 100;

	ret = dev_priv->ops->backlight_init(dev);
	if (ret)
		return ret;

	if (!acpi_video_backlight_use_native()) {
		drm_info(dev, "Skipping %s backlight registration\n",
			 dev_priv->ops->backlight_name);
		return 0;
	}

	if (IS_ENABLED(CONFIG_DRM_LUMINANCE))
		return drm_connector_attach_luminance_property(connector, 0,
							 PSB_MAX_BRIGHTNESS,
							 dev_priv->backlight_level);

	{
		struct backlight_properties props = {};

		props.brightness = dev_priv->backlight_level;
		props.max_brightness = PSB_MAX_BRIGHTNESS;
		props.type = BACKLIGHT_RAW;

		dev_priv->backlight_device =
			backlight_device_register(dev_priv->ops->backlight_name,
						  dev->dev, dev,
						  &gma_backlight_ops, &props);
		if (IS_ERR(dev_priv->backlight_device))
			return PTR_ERR(dev_priv->backlight_device);
	}

	return 0;
}

void gma_connector_set_luminance(const struct drm_connector_state *conn_state)
{
	gma_backlight_set(conn_state->connector->dev, conn_state->luminance);
}

void gma_connector_disable_luminance(const struct drm_connector_state *conn_state)
{
	gma_backlight_disable(conn_state->connector->dev);
}

void gma_backlight_exit(struct drm_device *dev)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);

	if (dev_priv->backlight_device)
		backlight_device_unregister(dev_priv->backlight_device);
}
