// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2024 Hisilicon Limited.

#include <linux/io.h>

#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_modes.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>

#include "hibmc_drm_drv.h"
#include "hibmc_drm_regs.h"
#include "dp/dp_hw.h"

#define HIBMC_DP_MASKED_SINK_HPD_PLUG_INT	BIT(2)
#define HIBMC_DP_MASKED_SINK_HPD_UNPLUG_INT	BIT(3)

static int hibmc_dp_connector_get_modes(struct drm_connector *connector)
{
	int count;
	struct hibmc_dp *dp = to_hibmc_dp(connector);
	struct edid *edid;

	edid = drm_get_edid(connector, &dp->aux.ddc);
	if (edid) {
		drm_connector_update_edid_property(connector, edid);
		count = drm_add_edid_modes(connector, edid);
		if (count) {
			dp->is_connected = true;
			goto out;
		}
	}

	dp->is_connected = false;

	count = drm_add_modes_noedid(connector,
				     connector->dev->mode_config.max_width,
				     connector->dev->mode_config.max_height);
	drm_set_preferred_mode(connector, 1024, 768);

out:
	kfree(edid);

	return count;
}
 
static int hibmc_dp_mode_valid(struct drm_connector *connector,
			       struct drm_display_mode *mode,
			       struct drm_modeset_acquire_ctx *ctx,
			       enum drm_mode_status *status)
{
	struct hibmc_dp *dp = to_hibmc_dp(connector);
	u64 cur_val, max_val;

	if (!dp->is_connected) {
		*status = MODE_OK;
		return 0;
	}

	cur_val = (u64)mode->htotal * mode->vtotal * drm_mode_vrefresh(mode) * BPP_24;
	max_val = (u64)hibmc_dp_get_link_rate(dp) * DP_MODE_VALI_CAL * hibmc_dp_get_lanes(dp);
	if (cur_val > max_val)
		*status = MODE_CLOCK_HIGH;
	else
		*status = MODE_OK;

	return 0;
}

static int hibmc_dp_detect(struct drm_connector *connector,
			   struct drm_modeset_acquire_ctx *ctx, bool force)
{
	struct hibmc_dp *dp = to_hibmc_dp(connector);

	if (dp->hpd_status)
		return connector_status_connected;
	else
		return connector_status_disconnected;
}

static const struct drm_connector_helper_funcs hibmc_dp_conn_helper_funcs = {
	.get_modes = hibmc_dp_connector_get_modes,
	.mode_valid_ctx = hibmc_dp_mode_valid,
	.detect_ctx = hibmc_dp_detect,
};

static int hibmc_dp_late_register(struct drm_connector *connector)
{
	struct hibmc_dp *dp = to_hibmc_dp(connector);

	hibmc_dp_enable_int(dp);

	return drm_dp_aux_register(&dp->aux);
}

static void hibmc_dp_early_unregister(struct drm_connector *connector)
{
	struct hibmc_dp *dp = to_hibmc_dp(connector);

	drm_dp_aux_unregister(&dp->aux);

	hibmc_dp_disable_int(dp);
}

static const struct drm_connector_funcs hibmc_dp_conn_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
	.late_register = hibmc_dp_late_register,
	.early_unregister = hibmc_dp_early_unregister,
};

static inline int hibmc_dp_prepare(struct hibmc_dp *dp, struct drm_display_mode *mode)
{
	int ret;

	hibmc_dp_display_en(dp, false);

	ret = hibmc_dp_mode_set(dp, mode);
	if (ret)
		drm_err(dp->drm_dev, "hibmc dp mode set failed: %d\n", ret);

	return ret;
}

static void hibmc_dp_encoder_enable(struct drm_encoder *drm_encoder,
				    struct drm_atomic_state *state)
{
	struct hibmc_dp *dp = container_of(drm_encoder, struct hibmc_dp, encoder);
	struct drm_display_mode *mode = &drm_encoder->crtc->state->mode;

	if (hibmc_dp_prepare(dp, mode))
		return;

	hibmc_dp_display_en(dp, true);
}

static void hibmc_dp_encoder_disable(struct drm_encoder *drm_encoder,
				     struct drm_atomic_state *state)
{
	struct hibmc_dp *dp = container_of(drm_encoder, struct hibmc_dp, encoder);

	hibmc_dp_display_en(dp, false);
}

static void hibmc_dp_encoder_mode_set(struct drm_encoder *encoder,
				      struct drm_crtc_state *crtc_state,
				      struct drm_connector_state *conn_state)
{
	u32 reg;
	struct drm_device *dev = encoder->dev;
	struct hibmc_drm_private *priv = to_hibmc_drm_private(dev);

	reg = readl(priv->mmio + HIBMC_DISPLAY_CONTROL_HISILE);
	reg |= HIBMC_DISPLAY_CONTROL_FPVDDEN(1);
	reg |= HIBMC_DISPLAY_CONTROL_PANELDATE(1);
	reg |= HIBMC_DISPLAY_CONTROL_FPEN(1);
	reg |= HIBMC_DISPLAY_CONTROL_VBIASEN(1);
	writel(reg, priv->mmio + HIBMC_DISPLAY_CONTROL_HISILE);
}

static const struct drm_encoder_helper_funcs hibmc_dp_encoder_helper_funcs = {
	.atomic_enable = hibmc_dp_encoder_enable,
	.atomic_disable = hibmc_dp_encoder_disable,
	.atomic_mode_set = hibmc_dp_encoder_mode_set,
};

irqreturn_t hibmc_dp_hpd_isr(int irq, void *arg)
{
	struct drm_device *dev = (struct drm_device *)arg;
	struct hibmc_drm_private *priv = to_hibmc_drm_private(dev);
	struct hibmc_dp *dp = &priv->dp;
	int idx;

	if (!drm_dev_enter(dev, &idx))
		return -ENODEV;

	if (dp->hpd_status) { /* only check unplug int when the last status is HPD in */
		if ((dp->irq_status & HIBMC_DP_MASKED_SINK_HPD_UNPLUG_INT)) {
			drm_dbg_dp(dev, "HPD OUT isr occur!\n");
			hibmc_dp_reset_link(dp);
			dp->hpd_status = 0;
			if (dev->registered)
				drm_helper_hpd_irq_event(dev);
		} else {
			drm_dbg_dp(dev, "HPD OUT occur but err!\n");
		}
	} else {
		if (dp->irq_status & HIBMC_DP_MASKED_SINK_HPD_PLUG_INT) {
			drm_dbg_dp(&priv->dev, "HPD IN isr occur!\n");
			hibmc_dp_hpd_cfg(dp);
			dp->hpd_status = 1;
			if (dev->registered)
				drm_helper_hpd_irq_event(dev);
		} else {
			drm_dbg_dp(dev, "HPD IN occur but err!\n");
		}
	}

	drm_dev_exit(idx);

	return IRQ_HANDLED;
}

int hibmc_dp_init(struct hibmc_drm_private *priv)
{
	struct drm_device *dev = &priv->dev;
	struct drm_crtc *crtc = &priv->crtc;
	struct hibmc_dp *dp = &priv->dp;
	struct drm_connector *connector = &dp->connector;
	struct drm_encoder *encoder = &dp->encoder;
	int ret;

	dp->mmio = priv->mmio;
	dp->drm_dev = dev;

	ret = hibmc_dp_hw_init(&priv->dp);
	if (ret) {
		drm_err(dev, "hibmc dp hw init failed: %d\n", ret);
		return ret;
	}

	hibmc_dp_display_en(&priv->dp, false);

	encoder->possible_crtcs = drm_crtc_mask(crtc);
	ret = drm_simple_encoder_init(dev, encoder, DRM_MODE_ENCODER_TMDS);
	if (ret) {
		drm_err(dev, "init dp encoder failed: %d\n", ret);
		return ret;
	}

	drm_encoder_helper_add(encoder, &hibmc_dp_encoder_helper_funcs);

	ret = drm_connector_init(dev, connector, &hibmc_dp_conn_funcs,
					  DRM_MODE_CONNECTOR_DisplayPort);
	if (ret) {
		drm_err(dev, "init dp connector failed: %d\n", ret);
		return ret;
	}

	drm_connector_helper_add(connector, &hibmc_dp_conn_helper_funcs);

	drm_connector_attach_encoder(connector, encoder);

	connector->polled = DRM_CONNECTOR_POLL_HPD;

	return 0;
}
