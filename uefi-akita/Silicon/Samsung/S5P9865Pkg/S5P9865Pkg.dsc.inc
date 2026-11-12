##
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  #
  # Timer
  #
  USE_PHYSICAL_TIMER                         = 1

  #
  # KEP
  #
  HAS_CNTFRQ_EL0_EMPTY_ERRATA                = 0

!include SamsungPkg/SamsungPkg.dsc.inc

[BuildOptions]
  *_*_AARCH64_CC_FLAGS = -mcpu=cortex-x3 -march=armv9-a+aes+bf16+bti+crc+crypto+fp16+fp16fml+i8mm+lse+pauth+rcpc+rdm+sb+sha2+sha3+sm4+ssbs+sve2-aes+sve2-bitperm+sve2-sha3+sve2-sm4

[PcdsFixedAtBuild]
  #
  # CPU
  #
  gArmPlatformTokenSpaceGuid.PcdCoreCount|9
  gArmPlatformTokenSpaceGuid.PcdClusterCount|3

  #
  # GIC
  #
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x10400000
  gArmTokenSpaceGuid.PcdGicRedistributorsBase|0x10440000

  #
  # RAM Manager
  #
  gSamsungPkgTokenSpaceGuid.PcdHasSecurePgtbl|TRUE
  gSamsungPkgTokenSpaceGuid.PcdHasHarx|TRUE
  gSamsungPkgTokenSpaceGuid.PcdHasDrmPlugin|TRUE

  #
  # GPIO
  # List Contents: 0x10840000, 0x10C40000, 0x12040000, 0x13040000, 0x13060000, 0x15060000, 0x154D0000, 0x154E0000
  #
  gSamsungPkgTokenSpaceGuid.PcdGpioCtrlAddr|{ 0x00, 0x00, 0x84, 0x10, 0x00, 0x00, 0xC4, 0x10, 0x00, 0x00, 0x04, 0x12, 0x00, 0x00, 0x04, 0x13, 0x00, 0x00, 0x06, 0x13, 0x00, 0x00, 0x06, 0x15, 0x00, 0x00, 0x4D, 0x15, 0x00, 0x00, 0x4E, 0x15 }

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorModel|"Google Tensor (TM) G3 @ 2.91 GHz"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorPartNumber|"S5P9865"

[LibraryClasses]
  #
  # SEC Libraries
  #
  PlatformSecLib|S5P9865Pkg/Library/PlatformSecLib/PlatformSecLib.inf

  #
  # IO Libraries
  #
  GpioBankLib|S5P9865Pkg/Library/GpioBankLib/GpioBankLib.inf

  #
  # UART Serial Libraries
  #
  #
  # The UART0 Console at 0x10870000 is left unclocked by the Production Boot
  # Loader, so DEBUG Builds keep SiliciumPkg's Frame Buffer Serial Port and
  # print to the Panel. Select S5P9865Pkg/Library/SerialPortLib for UART Logs.
  #

  #
  # Storage Libraries
  #
  PlatformUfsLib|S5P9865Pkg/Library/PlatformUfsLib/PlatformUfsLib.inf
