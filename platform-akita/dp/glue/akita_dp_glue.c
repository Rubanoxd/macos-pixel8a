/* SPDX-License-Identifier: GPL-2.0-or-later */
/* macOS 26.5 on the Pixel 8a (akita) - port by @1421rubano */
/* akita (Generated-by: LLM): glue between AkitaDp.inc (C++, AkitaAcpm) and Google's DisplayPort CAL and Synopsys
 * USB/DP PHY code, built unchanged as C (GPL-2.0, kept out of the public repo). */
#include "akita_linux_shim.h"
#include "cal_config.h"
#include "dp_cal.h"
#include "phy-samsung-usb-cal.h"

int akita_dpcal_debug = 1;
int akita_tca_ignore_timeout = 0;
int akita_dp_keep_acbyp = 1;   /* fullos306: HBR2 trains only with the AC-capacitor bypass kept */
unsigned akita_dp_req_mask = 0xF;   /* DP_CONFIG12 lanes asked to update (Google: all four) */
static struct exynos_usbphy_info g_phy;
static struct dp_hw_config g_cfg;
static int g_phy_on;

int phy_exynos_snps_usbdp_phy_enable(struct exynos_usbphy_info *info);
void phy_exynos_snps_usbdp_phy_disable(struct exynos_usbphy_info *info);

/* dp_hw_init() turns the combo PHY on through the USB driver; on akita that is us */
int dwc3_exynos_phy_enable(int owner, bool on)
{
	(void)owner;
	if (on) {
		if (g_phy_on) { pr_info("combo PHY already on\n"); return 0; }
		int r = phy_exynos_snps_usbdp_phy_enable(&g_phy);
		pr_info("combo PHY enable: %d\n", r);
		g_phy_on = 1;
		return r;
	}
	phy_exynos_snps_usbdp_phy_disable(&g_phy);
	g_phy_on = 0;
	return 0;
}

void akita_dp_setup(void *link, void *phy, void *tca, int flipped, int link_rate)
{
	dp_regs_desc_init(link, 0x110F0000, "LINK", REGS_LINK, SST1);
	dp_regs_desc_init(phy, 0x11130000, "PHY", REGS_PHY, SST1);
	dp_regs_desc_init(tca, 0x11140000, "PHY TCA", REGS_PHY_TCA, SST1);
	g_phy.regs_base = phy;
	g_phy.regs_base_2nd = tca;
	g_phy.version = 0x801;            /* zuma-usb.dtsi sub_phy_version */
	g_phy.usbdp_mode = SNPS_USBDP_RAM_MODE;   /* usbdp_mode = <1> */
	g_phy.used_phy_port = flipped;
	g_phy.tune_param = NULL;
	g_cfg.pin_type = PIN_TYPE_D;      /* the hub's pin assignment: 2 DP lanes + USB */
	g_cfg.orient_type = flipped ? PLUG_FLIPPED : PLUG_NORMAL;
	g_cfg.link_rate = (enum dp_link_rate_type)link_rate;
	g_cfg.num_lanes = 2;
	g_cfg.phy_boost = true;   /* Google exynos_drm_dp.c dp_phy_boost default */
	g_cfg.use_ssc = false;
}

int akita_dp_phy_enable(void) { return dwc3_exynos_phy_enable(1, 1); }
void akita_dp_phy_forget(void) { g_phy_on = 0; }
void akita_dp_set_lanes(int four) { g_cfg.pin_type = four ? PIN_TYPE_C : PIN_TYPE_D; g_cfg.num_lanes = four ? 4 : 2; }
int akita_dp_hw_init(void) { return dp_hw_init(&g_cfg); }
int akita_dp_read_dpcd(unsigned addr, unsigned len, unsigned char *buf) { return dp_hw_read_dpcd_burst(addr, len, buf); }
int akita_dp_read_edid(unsigned char *buf) { return dp_hw_read_edid(0, 128, buf); }

/* akita: step C (link training and BIST colour bars), the hardware half of Google's exynos_drm_dp.c */
int akita_dp_write_dpcd(unsigned addr, unsigned len, unsigned char *buf) { return dp_hw_write_dpcd_burst(addr, len, buf); }
void akita_dp_set_rate(int rate) { g_cfg.link_rate = (enum dp_link_rate_type)rate; }
int akita_dp_get_rate(void) { return (int)g_cfg.link_rate; }
void akita_dp_set_num_lanes(unsigned n) { g_cfg.num_lanes = n; }
void akita_dp_set_enhanced(int on) { g_cfg.enhanced_mode = on ? true : false; }
int akita_dp_reinit(void) { return dp_hw_reinit(&g_cfg); }
void akita_dp_training_pattern(int tps) { dp_hw_set_training_pattern((dp_training_pattern)tps); }
void akita_dp_swing(unsigned char *voltage, unsigned char *pre) { dp_hw_set_voltage_and_pre_emphasis(&g_cfg, voltage, pre); }

/* 1080p60 (CEA-861 VIC 16): 148.5 MHz, 2200 x 1125, porches H 88/148 (sync 44), V 4/36 (sync 5), positive syncs */
void akita_dp_timing_1080p60(void)
{
	struct video_timing *v = &g_cfg.vtiming;
	v->clock = 148500; v->htotal = 2200; v->vtotal = 1125;
	v->hfp = 88; v->hbp = 148; v->hactive = 1920;
	v->vfp = 4; v->vbp = 36; v->vactive = 1080;
	v->hsync_polarity = SYNC_POSITIVE; v->vsync_polarity = SYNC_POSITIVE;
	g_cfg.bpc = BPC_8;
	g_cfg.range = VESA_RANGE;
}
int akita_dp_bist_on(void)
{
	g_cfg.bist_mode = true;
	g_cfg.bist_type = COLOR_BAR;
	int r = dp_hw_set_bist_video_config(&g_cfg);
	if (!r) dp_hw_start();
	return r;
}
void akita_dp_video_stop(void) { dp_hw_stop(); }
void akita_dp_set_ssc(int on) { g_cfg.use_ssc = on ? true : false; }
/* step D: the DP link takes its pixels from DPIF (DECON1) instead of the BIST generator */
void akita_dp_bist_off(void);
int akita_dp_video_on(void)
{
	g_cfg.bist_mode = false;
	akita_dp_bist_off();
	dp_hw_set_video_config(&g_cfg);
	dp_hw_start();
	return 0;
}
/* hot-plug: the DP link and the combo PHY off (Google's dp_hw_deinit: interrupts, SW functions, OSC clock, PHY disable) */
int akita_dp_hw_deinit(void) { dp_hw_deinit(&g_cfg); return 0; }
