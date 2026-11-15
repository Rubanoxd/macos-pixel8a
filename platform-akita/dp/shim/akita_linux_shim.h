/* akita (Generated-by: LLM, local only): the few Linux kernel interfaces Google's DisplayPort CAL and Synopsys PHY code use,
 * mapped onto xnu. Lets cal_9865_dp_reg.c and phy-exynos-snps-usbdp.c build unchanged as freestanding C for AkitaAcpm. */
#ifndef AKITA_LINUX_SHIM_H
#define AKITA_LINUX_SHIM_H
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long long u64;
typedef signed char s8; typedef short s16; typedef int s32; typedef long long s64;
typedef unsigned long size_t; typedef unsigned long long phys_addr_t; typedef unsigned long long dma_addr_t; typedef unsigned long uintptr_t;
typedef _Bool bool;
#define true 1
#define false 0
#define NULL ((void *)0)
#define __iomem
#define __packed __attribute__((packed))
#define USEC_PER_SEC 1000000UL
#define MSEC_PER_SEC 1000UL
#define ENOENT 2
#define ENOMEM 12
#define ENOTSUPP 524
#define __maybe_unused __attribute__((unused))
typedef struct { volatile int counter; } atomic_t;
#define atomic_inc(a) __atomic_add_fetch(&(a)->counter, 1, __ATOMIC_SEQ_CST)
#define atomic_dec(a) __atomic_sub_fetch(&(a)->counter, 1, __ATOMIC_SEQ_CST)
#define atomic_read(a) ((a)->counter)
#define atomic_set(a, v) ((a)->counter = (v))
#define BIT(n) (1UL << (n))
#define GENMASK(h, l) (((~0U) << (l)) & (~0U >> (31 - (h))))
#define GENMASK_ULL(h, l) (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))
#define BIT_ULL(n) (1ULL << (n))
typedef unsigned int uint32_t; typedef unsigned short uint16_t; typedef unsigned char uint8_t; typedef unsigned long long uint64_t; typedef int int32_t;
typedef u16 __be16; typedef u32 __be32;
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((__typeof__(x))(a) - 1))
#define test_bit(nr, addr) ((*(const unsigned long *)(addr) >> (nr)) & 1UL)
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define EINVAL 22
#define ETIME 62
#define ETIMEDOUT 60
#define EIO 5
#define EBUSY 16
#define ENODEV 19
#define EAGAIN 35

extern int printf(const char *, ...) __attribute__((format(printf, 1, 2)));
extern void IODelay(unsigned microseconds);
extern void IOSleep(unsigned milliseconds);
static inline void udelay(unsigned long us) { IODelay((unsigned)us); }
static inline void mdelay(unsigned long ms) { IODelay((unsigned)(ms * 1000)); }
static inline void usleep_range(unsigned long a, unsigned long b) { (void)b; IODelay((unsigned)a); }
static inline void msleep(unsigned ms) { IOSleep(ms); }
static inline u32 readl(const volatile void *p) { u32 v = *(const volatile u32 *)p; __asm__ volatile("dsb ld" ::: "memory"); return v; }
static inline void writel(u32 v, volatile void *p) { __asm__ volatile("dsb st" ::: "memory"); *(volatile u32 *)p = v; }
#define readl_relaxed readl
#define writel_relaxed writel
#define readl_poll_timeout_atomic(addr, val, cond, delay_us, timeout_us) ({ \
	unsigned long __left = (timeout_us); int __r = 0; \
	for (;;) { (val) = readl(addr); if (cond) break; \
		if ((long)__left <= 0) { (val) = readl(addr); __r = (cond) ? 0 : -ETIMEDOUT; break; } \
		udelay(delay_us ? delay_us : 1); __left -= (delay_us ? delay_us : 1); } __r; })
#define readl_poll_timeout readl_poll_timeout_atomic
#define pr_info(fmt, ...) printf("dpcal: " fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...) printf("dpcal: ERR " fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...) printf("dpcal: WARN " fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...) do { if (akita_dpcal_debug) printf("dpcal: " fmt, ##__VA_ARGS__); } while (0)
extern int akita_dpcal_debug;
#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)
#define WARN_ON(x) (x)
#define unlikely(x) (x)
#define likely(x) (x)
static inline void *vmalloc(unsigned long n) { (void)n; return NULL; }
static inline void vfree(void *p) { (void)p; }
#endif
