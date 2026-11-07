// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM8550 GCC registers used by UFS, transcribed as facts from the public
// Linux gcc-sm8550.c clock driver. No write is performed by NativeUfs yet.
#ifndef NATIVE_UFS_GCC_UFS_H
#define NATIVE_UFS_GCC_UFS_H

// akita (Tensor G3, zuma): the Exynos UFS host the firmware (Mu UfsDxe) left linked up. Values from
// Mu-Silicium S5P9865Pkg PlatformUfsLib/BoardInit.c and SamsungPkg UfsDxe. The SM8550 GCC windows
// below are kept for reference only: the GCC/SMMU probes are refused on zuma (NativeUfs::start).
#define NATIVE_UFS_ZUMA               1
#define UFS_HC_BASE                   0x13200000ULL
#define UFS_HC_SIZE                   0x1000ULL       // UFSHCI registers, including the nexus clear at 0x140
#define UFS_VS_BASE                   0x13201100ULL   // Exynos vendor-specific block
#define UFS_VS_SIZE                   0x1000ULL
#define UFS_VS_TXPRDT_ENTRY_SIZE      0x000
#define UFS_VS_RXPRDT_ENTRY_SIZE      0x004
#define UFS_VS_UTRL_NEXUS_TYPE        0x040
#define UFS_HC_NEXUS_CLEAR            0x140           // bit 0: clear the NOP/QUERY nexus (Mu UfsUtpWaitResponse)
#define UFS_SG_SEGMENT_SHIFT          12              // PRDT entries hold at most 4 KiB (VS_*PRDT_ENTRY_SIZE)
#define UFS_SG_SEGMENT                (1U << UFS_SG_SEGMENT_SHIFT)
// SM8550 reference (unused on zuma).
#define GCC_BASE                      0x00100000ULL

#define GCC_UFS_PHY_GDSCR             0x77004
#define GCC_UFS_MEM_PHY_GDSCR         0x9e000

#define GCC_UFS_PHY_AXI_ENABLE        0x77018
#define GCC_UFS_PHY_AHB_ENABLE        0x77024
#define GCC_UFS_PHY_TX_SYMBOL0_ENABLE 0x77028
#define GCC_UFS_PHY_RX_SYMBOL0_ENABLE 0x7702c
#define GCC_UFS_PHY_UNIPRO_CORE_ENABLE 0x77068
#define GCC_UFS_PHY_ICE_CORE_ENABLE   0x77074
#define GCC_UFS_PHY_PHY_AUX_ENABLE    0x770b0
#define GCC_UFS_PHY_RX_SYMBOL1_ENABLE 0x770cc
// bus_aggr_clk in the board DT: gcc_aggre_ufs_phy_axi_clk, halt/enable 0x770e4, HW clock gating bit1
#define GCC_AGGRE_UFS_PHY_AXI_ENABLE  0x770e4

#define GCC_UFS_BRANCH_ENABLE_BIT     0x1U

// gcc_ufs_phy_ahb_clk and gcc_aggre_ufs_phy_axi_clk set hwcg_bit=1 in gcc-sm8550.c, so CLK_OFF=1 on
// an idle branch is normal (hardware 2026-10-17 read ahb=80000003).
// The reset and RPMh/TCSR/QMP-PHY programming are not represented here: their
// semantics are not qualified and NativeUfs must not write them speculatively.
#endif
