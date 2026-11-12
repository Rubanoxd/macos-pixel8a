#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/UfsHostBridge.h>

#include <Protocol/EFIGpio.h>

//
// UFS Pins
//
// Both live in the HSI2UFS Pin Controller as Bank gph5. The Boot Loader
// leaves them muxed, but it also quiesces UFS before handing over, so they
// are configured again here.
//
#define ZUMA_UFS_GPIO_BANK      5
#define ZUMA_UFS_REFCLK_OUT_PIN 0
#define ZUMA_UFS_RST_N_PIN      1

//
// Device Power
//
// The Device Tree calls the Supply a Fixed Regulator, but it is switched by
// a GPIO: "ufs-vcc" is gpp0-1 in the PERIC0 Pin Controller, Active High.
// This is the one Value here that a future Board could route differently.
// UfsDxe drives it directly, so it needs the Address of the Data Register
// and the Bit within it.
//
#define ZUMA_PERIC0_BASE        0x10840000
#define ZUMA_GPP0_OFFSET        0x0000
#define ZUMA_GPIO_DAT_OFFSET    0x0004
#define ZUMA_UFS_VCC_BANK       0
#define ZUMA_UFS_VCC_PIN        1
#define ZUMA_UFS_VCC_DAT        (ZUMA_PERIC0_BASE + ZUMA_GPP0_OFFSET + ZUMA_GPIO_DAT_OFFSET)

STATIC EFI_GPIO_PROTOCOL *mGpioProtocol;

//
// UFS Register Blocks
//
// Addresses come from the zuma Device Tree "ufs@13200000" Node
// (reg-names = "hci", "vs_hci", "unipro", "ufsp") and the separate
// PHY Node "phy@13204000" (reg-names = "phy-pma").
//
// Note that the Block Layout differs from the older Exynos SoCs: the
// UNIPRO Block sits at a 0x80000 Offset instead of 0x8000, and 0x132A0000
// is the UFS Protector, not the PHY.
//
#define ZUMA_UFS_BASE               0x13200000
#define ZUMA_UFS_VS_BASE            0x13201100
#define ZUMA_UNIPRO_BASE            0x13280000
#define ZUMA_PHY_PMA_BASE           0x13204000

//
// UFS Vendor Specific Registers
//
// The UNIPRO Clock feeds the 1us Tick Counter and every Period the
// Calibration Data derives. Read back from the Boot Loader's Clock Setup:
// the Mux selects dout_cmu_shared0_div4 (the 2400 MHz shared0 PLL over two
// chained Halvings) and the Divider adds a Third, giving 2400 / 12.
//
#define UFS_SCLK                       200000000UL
#define CNT_VAL_1US_MASK               0x3FFU
#define UFSHCI_VS_1US_TO_CNT_VAL       0x110CU
#define UFSHCI_VS_UFSHCI_V2P1_CTRL     0x118CU
#define IA_TICK_SEL                    (1U << 16)

//
// PHY Isolation
//
// zuma and zuma insert extra Registers ahead of the Isolation Controls,
// so UFS PHY_CTRL sits one Register below the gs101 Offset of 0x3EC8.
//
#define ZUMA_PMU_BASE               0x15460000
#define ZUMA_PMU_UFS_PHY_CONTROL    (ZUMA_PMU_BASE + 0x3EC0)

//
// IO Coherency
//
#define ZUMA_SYSREG_UFS_BASE        0x13020000
#define ZUMA_SYSREG_UFS_IOCOHERENCY (ZUMA_SYSREG_UFS_BASE + 0x710)
#define ZUMA_SYSREG_UFS_IOCC_MASK   0x3U

//
// Clock Management Unit
//
// UFS_SCLK above is still an Assumption. The Boot Loader already programmed
// these, so dump them to work out the real Rate:
//   MUX selects oscclk, shared0_div4, shared2_div2 or the Spare PLL.
//
#define ZUMA_CMU_TOP_BASE           0x26040000
#define ZUMA_CMU_MUX_UFS_EMBD       (ZUMA_CMU_TOP_BASE + 0x10B8)
#define ZUMA_CMU_DIV_UFS_EMBD       (ZUMA_CMU_TOP_BASE + 0x18B0)
#define ZUMA_CMU_PLL_CON0_SHARED0   (ZUMA_CMU_TOP_BASE + 0x0100)
#define ZUMA_CMU_DIV_SHARED0_DIV4   (ZUMA_CMU_TOP_BASE + 0x1908)

STATIC
VOID
UfsVsSet1usToCnt (struct UfsHost *Ufs)
{
  UINT32 Register = MmioRead32 ((UINTN)(Ufs->IoAddr + UFSHCI_VS_UFSHCI_V2P1_CTRL));

  Register |= IA_TICK_SEL;

  MmioWrite32 ((UINTN)(Ufs->IoAddr + UFSHCI_VS_UFSHCI_V2P1_CTRL), Register);
  MmioWrite32 ((UINTN)(Ufs->IoAddr + UFSHCI_VS_1US_TO_CNT_VAL), (UFS_SCLK / 1000000) & CNT_VAL_1US_MASK);
}

STATIC
EFI_STATUS
UfsConfigurePins ()
{
  EFI_STATUS Status;
  CONST UINT8 Pins[] = { ZUMA_UFS_REFCLK_OUT_PIN, ZUMA_UFS_RST_N_PIN };

  // Locate GPIO Protocol
  Status = gBS->LocateProtocol (&gEfiGpioProtocolGuid, NULL, (VOID *)&mGpioProtocol);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Locate GPIO Protocol! Status = %r\n", Status));
    return Status;
  }

  // Configure REFCLK_OUT & RST_N
  for (UINTN Index = 0; Index < ARRAY_SIZE (Pins); Index++) {
    Status = mGpioProtocol->SetPull (BANK_ID_H, ZUMA_UFS_GPIO_BANK, Pins[Index], PULL_NONE);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "Failed to set Pull for UFS Pin %u! Status = %r\n", Pins[Index], Status));
      return Status;
    }

    Status = mGpioProtocol->SetFunction (BANK_ID_H, ZUMA_UFS_GPIO_BANK, Pins[Index], FUNCTION_2);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "Failed to set Function for UFS Pin %u! Status = %r\n", Pins[Index], Status));
      return Status;
    }
  }

  // Drive the Device Supply Enable
  Status = mGpioProtocol->SetFunction (BANK_ID_P, ZUMA_UFS_VCC_BANK, ZUMA_UFS_VCC_PIN, FUNCTION_OUTPUT);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to configure UFS Supply Enable! Status = %r\n", Status));
    return Status;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
UfsBoardInit (struct UfsHost *Ufs)
{
  EFI_STATUS Status;
  UINT32     Register;

  DEBUG ((EFI_D_INFO, "UFS: Board init\n"));

  // Configure UFS Pins
  Status = UfsConfigurePins ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Set UFS Register Blocks
  Ufs->IoAddr     = (VOID *)(UINTN)ZUMA_UFS_BASE;
  Ufs->VsAddr     = (VOID *)(UINTN)ZUMA_UFS_VS_BASE;
  Ufs->UniProAddr = (VOID *)(UINTN)ZUMA_UNIPRO_BASE;
  Ufs->PhyPma     = (VOID *)(UINTN)ZUMA_PHY_PMA_BASE;

  // Set PHY Isolation Address
  Ufs->PhyIsoAddr = (VOID *)(UINTN)ZUMA_PMU_UFS_PHY_CONTROL;

  // Set the Device Supply Enable
  Ufs->DevPwrAddr  = (VOID *)(UINTN)ZUMA_UFS_VCC_DAT;
  Ufs->DevPwrShift = ZUMA_UFS_VCC_PIN;

  // Set Link Parameters
  Ufs->MclkRate = UFS_SCLK;

  //
  // The Board feeds the Device a 38.4 MHz Reference Clock, which is also what
  // the Physical Coding Sublayer is programmed for in the Calibration Data.
  //
  Ufs->RefClkFreq = UFS_REF_CLK_38_4_MHZ;

  //
  // This Controller counts the Descriptor Offsets and Lengths in Double
  // Words, which is what the Device Tree means by "fixed-prdt-req_list-ocs".
  //
  Ufs->Quirks &= ~UFS_QUIRK_PRDT_BYTE_GRAN;

  //
  // Gear 4 needs Adapt, which the Calibration Data selects per Gear.
  //
  Ufs->GearMode = 4;

  // Report the Boot Loader's Clock Setup so UFS_SCLK can be verified
  DEBUG ((EFI_D_INFO, "UFS: CMU MUX = 0x%08x, DIV = 0x%08x, SHARED0 PLL = 0x%08x, SHARED0_DIV4 = 0x%08x\n",
          MmioRead32 (ZUMA_CMU_MUX_UFS_EMBD),
          MmioRead32 (ZUMA_CMU_DIV_UFS_EMBD),
          MmioRead32 (ZUMA_CMU_PLL_CON0_SHARED0),
          MmioRead32 (ZUMA_CMU_DIV_SHARED0_DIV4)));

  // Configure the 1us Tick Counter
  UfsVsSet1usToCnt (Ufs);

  //
  // The PHY is taken out of Isolation by UfsDxe itself, using the Address
  // set above.
  //

  // Enable IO Coherency
  Register = MmioRead32 (ZUMA_SYSREG_UFS_IOCOHERENCY);
  MmioWrite32 (ZUMA_SYSREG_UFS_IOCOHERENCY, Register | ZUMA_SYSREG_UFS_IOCC_MASK);

  return EFI_SUCCESS;
}
