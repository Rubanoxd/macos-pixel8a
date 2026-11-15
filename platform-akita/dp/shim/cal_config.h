#ifndef AKITA_CAL_CONFIG_H
#define AKITA_CAL_CONFIG_H
#include "linux/types.h"
#define cal_log_debug(id, fmt, ...) pr_debug("%s: " fmt, __func__, ##__VA_ARGS__)
#define cal_log_info(id, fmt, ...) pr_info("%s: " fmt, __func__, ##__VA_ARGS__)
#define cal_log_warn(id, fmt, ...) pr_warn("%s: " fmt, __func__, ##__VA_ARGS__)
#define cal_log_err(id, fmt, ...) pr_err("%s: " fmt, __func__, ##__VA_ARGS__)
#define CONFIG_SOC_ZUMA 1
struct cal_regs_desc { const char *name; void __iomem *regs; phys_addr_t start; bool write_protected; };
static inline u32 cal_read(struct cal_regs_desc *d, u32 off) { return readl((volatile u8 *)d->regs + off); }
static inline void cal_write(struct cal_regs_desc *d, u32 off, u32 v) { writel(v, (volatile u8 *)d->regs + off); }
static inline u32 cal_read_mask(struct cal_regs_desc *d, u32 off, u32 mask) { return cal_read(d, off) & mask; }
static inline void cal_write_mask(struct cal_regs_desc *d, u32 off, u32 v, u32 mask) {
	u32 old = cal_read(d, off); cal_write(d, off, (v & mask) | (old & ~mask)); }
#define cal_regs_desc_check(type, id, type_max, id_max) do { } while (0)
#define cal_regs_desc_set(descs, r, s, n, type, id) do { (descs)[type][id].regs = (r); (descs)[type][id].start = (s); (descs)[type][id].name = (n); } while (0)
/* the secure (HDCP/CRC) SMC paths are never taken on akita: they fail without calling the monitor */
#define SMC_DRM_DPU_CRC_SEC 0
#define DRMDRV_OK 0
#define SMC_DRM_HISTOGRAM_SEC 0
static inline unsigned long exynos_smc4(unsigned long a, unsigned long b, unsigned long c, unsigned long d, unsigned long *r, unsigned long e, unsigned long f) { (void)a; (void)b; (void)c; (void)d; (void)r; (void)e; (void)f; return 1; }
#endif
