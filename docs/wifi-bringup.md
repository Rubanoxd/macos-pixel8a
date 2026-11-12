# Wi-Fi bring-up (in progress)

The Pixel 8a's Wi-Fi is a **Broadcom BCM4383 rev A3** (Wi-Fi 6E) on PCIe channel 1 (`pcie@13120000`, one lane). Apple's Wi-Fi
drivers only know the chips in Macs, so it needs its own driver. The plan is a FullMAC driver (the chip's firmware does 802.11,
scanning and WPA) presented to macOS as an Ethernet interface.

Code: `platform-akita/drivers/Acpm/AkitaPcie.inc`, staged through `sysctl kern.akita_acpm=4000000+stage` (boot-arg `akita-acpm=1`).
Reference: Google's `pcie-exynos-rc.c` / `pcie-exynos-zuma-rc-cal.c` (soc/gs) and `bcmdhd/bcm4383` (both GPL, read for the
sequences and register values).

| Stage | What | Result (fullos195–197) |
|---|---|---|
| 1 | PMU PHY control word (secure SMC read), gph3 pins | PHY isolated (`0xc0`), WLAN_EN off: the bootloader leaves Wi-Fi off |
| 2 | Read the controller's ELBI/SOC windows | Readable: the block is clocked (HSI2, shared with UFS) |
| 3 | WLAN_EN high, PHY isolation off, PHY/PCS config, PERST, LTSSM | PLL, CDR and offset calibration lock; **link up (L0) in ~40 µs**, Gen1 x1 |
| 4 | iATU CFG0 window, endpoint config space | **`14e4:4449`, subsystem `0x438314e4`: the BCM4383** |

Next: assign BARs and read the chip's backplane (chipcommon ID) through BAR0; download the firmware
(`fw_bcmdhd.bin_4383_a3`, NVRAM and CLM blob from the phone's own vendor partition, never redistributed) into the chip's RAM and
start its ARM core; then the msgbuf rings, a scan and WPA2 association; finally an `IOEthernetController` for macOS.
