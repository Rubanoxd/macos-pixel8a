##
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = akita
  PLATFORM_GUID                  = 75A25568-3784-46E1-B03B-D24D5137BF34
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/akitaPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = akitaPkg/akita.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0

!include S5P9865Pkg/S5P9865Pkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # Debug Output
  #
  # The Default Mask leaves DEBUG_INFO off, which hides the Storage and Link
  # Bring-Up Messages. Remove the 0x40 Bit once UFS is stable.
  #
!if $(TARGET) == DEBUG
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x8007EE4F
!endif

  #
  # DDR Memory
  #
  # Base is the first DRAM Bank (2 GB at 0x80000000). It must stay at BASE_2GB:
  # RamManagerLib derives the whole Bank Layout from it (Bank 2 at 0x880000000).
  # akita has 8 GB: the live Device Tree lists memory@80000000, memory@880000000,
  # memory@900000000 and memory@980000000. Size is the total visible DRAM minus
  # 512 MB for the ABL Carveouts at the Top of DRAM, as on caimito. The Boot
  # Loader does not provide a Samsung SoC Info Structure, so RamManagerLib reads
  # this PCD instead.
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000
  gArmTokenSpaceGuid.PcdSystemMemorySize|0x200000000

  #
  # UEFI Stack
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0xA0212000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Google"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Pixel 8a"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"akita"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"Pixel_8a_akita"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"Pixel 8a"

  #
  # Simple Frame Buffer
  #
  #
  # 1080x2400, at the 0xFAC00000 Splash Buffer the Pixel Boot Loader also uses
  # on gs101 (Mainline gs101-oriole.dts, cont_splash_mem).
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1080
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2400
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|akitaPkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # Input Libraries
  #
  KeypadDeviceLib|akitaPkg/Library/KeypadDeviceLib/KeypadDeviceLib.inf

  #
  # UniXNU Boot Manager
  #
  UniXNULib|UniXNUPkg/Library/UniXNULib/UniXNULib.inf

[Components]
  #
  # Input
  #
  SiliciumPkg/Drivers/KeypadDxe/KeypadDxe.inf
  SiliciumPkg/Drivers/KeypadDeviceDxe/KeypadDeviceDxe.inf

  #
  # UniXNU: akita has no ESP, so the Boot Manager, its Boot Option Driver,
  # DarwinBoot and the initrd Block Device live in the Firmware Volume.
  #
  UniXNUPkg/Application/UniXNU/UniXNU.inf
  UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf
  UniXNUPkg/Driver/UniXNURamDiskDxe/UniXNURamDiskDxe.inf
  DarwinBootPkg/Driver/DarwinBoot.inf {
    <BuildOptions>
      # XNU console at 2x, unrotated (the legacy display refuses a rotated console).
      *_*_*_CC_FLAGS = -DDARWINBOOT_VIDEO_SCALE=2 -DDARWINBOOT_VIDEO_ROTATE=0
  }
