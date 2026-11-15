/* akita (Generated-by: LLM, local only): glue between AkitaDp.inc and Google's DECON/DPP CAL (cal_9865_decon_reg.c,
 * cal_9865_dpp_reg.c; GPL-2.0, built aside like the DP CAL). DECON1 drives the DP link through DPIF0 in video mode. */
#include "akita_linux_shim.h"
#include "cal_config.h"
#include "decon_cal.h"
#include "dpp_cal.h"
#include "regs-dpp.h"
#include "../exynos_drm_format.h"

/* debug helpers the CAL references from its dump functions (never called on akita: they read undocumented ranges) */
void dpu_print_hex_dump(struct drm_printer *p, void __iomem *regs, const void *buf, size_t len) { (void)p; (void)regs; (void)buf; (void)len; }
void cal_drm_printf(struct drm_printer *p, u32 id, const char *fmt, ...) { (void)p; (void)id; (void)fmt; }
void cal_set_write_protected(struct cal_regs_desc *d, bool protected) { d->write_protected = protected; }
void dqe_dump(struct drm_printer *p, u32 dqe_id) { (void)p; (void)dqe_id; }

/* the two formats macOS surfaces use: BGRA bytes in memory = DRM ARGB8888 / XRGB8888 (exynos_drm_format.c) */
static const struct dpu_fmt akita_fmts[] = {
	{ "ARGB8888", DRM_FORMAT_ARGB8888, IDMA_IMG_FORMAT_ARGB8888, DPP_IMG_FORMAT_ARGB8888, 32, 0, 8, 1, 8, DPU_COLORSPACE_RGB },
	/* fullos311: IDMA format 7 showed grey as blue and half the width stretched; DPP L0 reads the same UEFI/macOS BGRA bytes with
	 * format 0 (IDMA_IMG_FORMAT_BGRA8888 in this regs-dpp.h) and the panel looks right: use that */
	{ "XRGB8888", DRM_FORMAT_XRGB8888, IDMA_IMG_FORMAT_BGRA8888, DPP_IMG_FORMAT_ARGB8888, 24, 8, 8, 1, 0, DPU_COLORSPACE_RGB },
};
const struct dpu_fmt *dpu_find_fmt_info(u32 fourcc)
{
	for (unsigned i = 0; i < ARRAY_SIZE(akita_fmts); i++)
		if (akita_fmts[i].fmt == fourcc)
			return &akita_fmts[i];
	return &akita_fmts[1];
}

static struct decon_config g_dc;

void akita_decon1_map(void *main1, void *win, void *sub, void *wincon1)
{
	decon_regs_desc_init(main1, 0x19471000, "decon1", REGS_DECON, REGS_DECON1_ID);
	decon_regs_desc_init(win, 0x19480000, "decon_win", REGS_DECON_WIN, REGS_DECON1_ID);
	decon_regs_desc_init(sub, 0x19490000, "decon_sub", REGS_DECON_SUB, REGS_DECON1_ID);
	decon_regs_desc_init(wincon1, 0x194B0000, "decon_wincon", REGS_DECON_WINCON, REGS_DECON1_ID);
}

/* exynos_drm_decon.c decon_update_config for a DP connector, with zuma-drm-dpu.dtsi's urgent values */
static void akita_decon1_config(u32 w, u32 h)
{
	memset(&g_dc, 0, sizeof(g_dc));
	g_dc.out_type = DECON_OUT_DP0;
	g_dc.mode.op_mode = DECON_VIDEO_MODE;
	g_dc.mode.dsi_mode = DSI_MODE_NONE;
	g_dc.mode.trig_mode = DECON_SW_TRIG;
	g_dc.te_from = MAX_DECON_TE_FROM_DDI;
	g_dc.image_width = w;
	g_dc.image_height = h;
	g_dc.out_bpc = 8;
	g_dc.in_bpc = 8;
	g_dc.urgent.rd_en = 1;
	g_dc.urgent.rd_hi_thres = 0x800;
	g_dc.urgent.rd_lo_thres = 0x400;
	g_dc.urgent.rd_wait_cycle = 0x10;
	g_dc.main_dsim_id = -1;
}

/* step D1a: DECON1 in video mode to DPIF0 with window 0 as a solid colour (no DPP layer, no memory, no sysmmu) */
int akita_decon1_colormap(u32 w, u32 h, u32 win, u32 rgb)
{
	struct decon_window_regs wr;
	int r;

	akita_decon1_config(w, h);
	r = decon_reg_init(REGS_DECON1_ID, &g_dc);
	pr_info("decon1: init %d (%ux%u, DP0, video)\n", r, w, h);
	memset(&wr, 0, sizeof(wr));
	wr.start_pos = 0;
	wr.end_pos = ((h - 1) << 16) | (w - 1);   /* win_end_pos(): inclusive, Y in the high half */
	wr.colormap = rgb;
	wr.blend = DECON_BLENDING_NONE;
	wr.in_bpc = 8;
	decon_reg_set_window_control(REGS_DECON1_ID, win, &wr, true);
	decon_reg_update_req_window(REGS_DECON1_ID, win);
	r = decon_reg_start(REGS_DECON1_ID, &g_dc);
	pr_info("decon1: start %d\n", r);
	return r;
}

int akita_decon1_stop(void) { return decon_reg_stop(REGS_DECON1_ID, &g_dc, true, 60); }

/* step D1b: DPP L1 (channel 1, DPUF0; zuma-drm-dpu.dtsi drmdpp1: attr 0x6D0C7F) reads a framebuffer, DECON1 window shows it */
#define AKITA_DPP1_ATTR 0x6D0C7FUL
void akita_dpp1_map(void *dma, void *dpp, void *sramc, void *scl_coef, void *hdr_comm)
{
	dpp_regs_desc_init(dma, 0x19901000, "dma1", REGS_DMA, REGS_DPP1_ID);
	dpp_regs_desc_init(dpp, 0x19931000, "dpp1", REGS_DPP, REGS_DPP1_ID);
	dpp_regs_desc_init(sramc, 0x19951000, "sramc1", REGS_SRAMC, REGS_DPP1_ID);
	dpp_regs_desc_init(scl_coef, 0x19940000, "scl_coef", REGS_SCL_COEF, REGS_DPP1_ID);
	dpp_regs_desc_init(hdr_comm, 0x19961000, "hdr_comm1", REGS_HDR_COMM, REGS_DPP1_ID);
}

/* show (sx, sy, w, h) of an f_w x f_h XRGB8888 buffer at addr, 1:1, at (wx, wy) of DECON1's frame, through window win */
int akita_dpp1_show(u64 addr, u32 f_w, u32 f_h, u32 sx, u32 sy, u32 w, u32 h, u32 win, u32 wx, u32 wy)
{
	const unsigned long attr = AKITA_DPP1_ATTR;
	struct dpp_params_info p;
	struct decon_window_regs wr;

	memset(&p, 0, sizeof(p));
	p.src.x = sx; p.src.y = sy; p.src.w = w; p.src.h = h; p.src.f_w = f_w; p.src.f_h = f_h;
	p.dst.x = wx; p.dst.y = wy; p.dst.w = w; p.dst.h = h; p.dst.f_w = g_dc.image_width; p.dst.f_h = g_dc.image_height;
	p.rot = 0;
	p.format = DRM_FORMAT_XRGB8888;
	p.addr[0] = addr;
	p.h_ratio = 1 << 20; p.v_ratio = 1 << 20;
	p.is_scale = false;
	p.is_block = false;
	p.standard = EXYNOS_STANDARD_UNSPECIFIED;
	p.range = EXYNOS_RANGE_FULL;
	p.in_bpc = DPP_BPC_8;
	p.comp_type = COMP_TYPE_NONE;
	p.rcv_num = 400000;   /* exynos_devfreq DISP kHz (dfs_lv 400000): the DMA deadlock timer is rcv_num x 51 */
	pr_info("dpp1: RDMA_ENABLE before %#x IRQ %#x\n", dma_read(REGS_DPP1_ID, RDMA_ENABLE), dma_read(REGS_DPP1_ID, RDMA_IRQ));
	dpp_reg_init(REGS_DPP1_ID, attr);
	dpp_reg_configure_params(REGS_DPP1_ID, &p, attr);
	pr_info("dpp1: configured: IN_CTRL_0 %#x SRC %#x/%#x IMG_SIZE %#x BASE %#x\n", dma_read(REGS_DPP1_ID, RDMA_IN_CTRL_0),
		dma_read(REGS_DPP1_ID, RDMA_SRC_WIDTH), dma_read(REGS_DPP1_ID, RDMA_SRC_HEIGHT), dma_read(REGS_DPP1_ID, RDMA_IMG_SIZE),
		dma_read(REGS_DPP1_ID, RDMA_BASEADDR_P0));

	memset(&wr, 0, sizeof(wr));
	wr.start_pos = (wy << 16) | wx;
	wr.end_pos = ((wy + h - 1) << 16) | (wx + w - 1);
	wr.ch = REGS_DPP1_ID;   /* the DPP's id is its channel number */
	wr.plane_alpha = 0xff;
	wr.blend = DECON_BLENDING_NONE;
	wr.in_bpc = 8;
	decon_reg_set_window_control(REGS_DECON1_ID, win, &wr, false);
	decon_reg_update_req_window(REGS_DECON1_ID, win);
	decon_reg_update_req_global(REGS_DECON1_ID);
	return 0;
}
u32 akita_dpp1_irq(void) { return dma_read(REGS_DPP1_ID, RDMA_IRQ); }
