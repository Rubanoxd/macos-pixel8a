/**
  Copyright (c) 2011-2012, ARM Limited. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/IoLib.h>
#include <Library/PlatformSecLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/ArmSmcLib.h>
#include <Library/BaseMemoryLib.h>

#include "PlatformRegisters.h"

//
// Bring-Up Halt Point: 0 boots normally, 1 stops after the Watchdog, 2 paints
// the Panel red and stops.
//
#define AKITA_HALT_STAGE 0

//
// Bring-Up Auto Return: 1 keeps the Boot Loader's Watchdog running and sets the
// Reboot Mode to Bootloader, so a Hang anywhere later (UEFI, DarwinBoot, XNU)
// ends in fastboot about a Minute later without Buttons.
//
#define AKITA_AUTO_FASTBOOT 1

//
// The PMU is written through the Secure Monitor on Tensor, as Linux does
// (TENSOR_SMC_PMU_SEC_REG). The Reboot Command lives at PMU + 0x810
// (pixel-reboot reboot-cmd-offset); 0xFC selects the Bootloader.
//
#define TENSOR_SMC_PMU_SEC_REG  0x82000504
#define TENSOR_PMUREG_WRITE     1
#define ZUMA_PMU_BASE           0x15460000
#define ZUMA_PMU_REBOOT_CMD     (ZUMA_PMU_BASE + 0x0810)
#define REBOOT_MODE_BOOTLOADER  0xFC

//
// Exynos Watchdog Registers (s3c2410-wdt layout)
//
#define WTCON                   0x00
#define WTDAT                   0x04
#define WTCNT                   0x08
#define WTCON_RESET_ENABLE      (1 << 0)
#define WTCON_ENABLE            (1 << 5)
#define WTCON_DIV128            (3 << 3)
#define WTCON_PRESCALER_MAX     (0xFF << 8)

//
// The Boot Loader leaves the Cluster 0 Watchdog armed with a Timeout of a few
// Seconds ("APC Watchdog Early"). Re-arm it with the longest Period the Counter
// allows, and stop the Cluster 1 Watchdog, so a Hang later still resets.
//
VOID
ArmLongWatchdog ()
{
  EFI_STATUS                   Status;
  EFI_MEMORY_REGION_DESCRIPTOR WatchdogTimerCl0Region;
  EFI_MEMORY_REGION_DESCRIPTOR WatchdogTimerCl1Region;

  Status = LocateMemoryRegionByName ("Watchdog Timer CL1", &WatchdogTimerCl1Region);
  if (!EFI_ERROR (Status)) {
    MmioAnd32 (WatchdogTimerCl1Region.Address + WTCON, ~WATCHDOG_ENABLE);
  }

  Status = LocateMemoryRegionByName ("Watchdog Timer CL0", &WatchdogTimerCl0Region);
  if (!EFI_ERROR (Status)) {
    MmioWrite32 (WatchdogTimerCl0Region.Address + WTCON, 0);
    MmioWrite32 (WatchdogTimerCl0Region.Address + WTDAT, 0xFFFF);
    MmioWrite32 (WatchdogTimerCl0Region.Address + WTCNT, 0xFFFF);
    MmioWrite32 (WatchdogTimerCl0Region.Address + WTCON, WTCON_PRESCALER_MAX | WTCON_DIV128 | WTCON_ENABLE | WTCON_RESET_ENABLE);
  }
}

VOID
SetRebootModeBootloader ()
{
  ARM_SMC_ARGS Args;

  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = TENSOR_SMC_PMU_SEC_REG;
  Args.Arg1 = ZUMA_PMU_REBOOT_CMD;
  Args.Arg2 = TENSOR_PMUREG_WRITE;
  Args.Arg3 = REBOOT_MODE_BOOTLOADER;
  ArmCallSmc (&Args);
}

VOID
DisableWatchdogTimer ()
{
  EFI_STATUS                   Status;
  EFI_MEMORY_REGION_DESCRIPTOR WatchdogTimerCl0Region;
  EFI_MEMORY_REGION_DESCRIPTOR WatchdogTimerCl1Region;

  // 1. Locate and disable Cluster 0 Watchdog independently
  Status = LocateMemoryRegionByName ("Watchdog Timer CL0", &WatchdogTimerCl0Region);
  if (!EFI_ERROR (Status)) {
    MmioAnd32 (WatchdogTimerCl0Region.Address, ~WATCHDOG_ENABLE);
  }

  // 2. Locate and disable Cluster 1 Watchdog independently
  Status = LocateMemoryRegionByName ("Watchdog Timer CL1", &WatchdogTimerCl1Region);
  if (!EFI_ERROR (Status)) {
    MmioAnd32 (WatchdogTimerCl1Region.Address, ~WATCHDOG_ENABLE);
  }
}

VOID
EnableFrameBufferWrites ()
{
  EFI_STATUS                   Status;
  EFI_MEMORY_REGION_DESCRIPTOR DrmDeconRegion;

  // Locate "DRM Decon" Memory Region
  Status = LocateMemoryRegionByName ("DRM Decon", &DrmDeconRegion);
  if (EFI_ERROR (Status)) {
    return;
  }

  // Configure DRM Decon
  for (UINT16 Length = 0; Length < DrmDeconRegion.Length; Length += 0x1000) {
    MmioWrite32 (DrmDeconRegion.Address + Length + HW_SW_TRIG_CONTROL, 0x3061);
  }
}

VOID
PlatformInitialize ()
{
#if AKITA_AUTO_FASTBOOT
  // Return to fastboot when the long Watchdog fires
  SetRebootModeBootloader ();
  ArmLongWatchdog ();
#else
  // Disable Watchdog Timer
  DisableWatchdogTimer ();
#endif

#if defined (AKITA_HALT_STAGE) && AKITA_HALT_STAGE == 1
  // Bring-Up: stop before touching the Display. A Device that stays off USB
  // reached this Point.
  for (;;) {
    __asm__ volatile ("wfe");
  }
#endif

  // Enable Frame Buffer Writes
  EnableFrameBufferWrites ();

#if defined (AKITA_HALT_STAGE) && AKITA_HALT_STAGE == 2
  // Bring-Up: paint the Frame Buffer solid Red and stop. A red Panel proves
  // the Splash Buffer Address and the DECON Trigger.
  for (UINTN Offset = 0; Offset < 1080 * 2400 * 4; Offset += 4) {
    MmioWrite32 (0xFAC00000 + Offset, 0xFFFF0000);
  }

  for (;;) {
    __asm__ volatile ("wfe");
  }
#endif
}
