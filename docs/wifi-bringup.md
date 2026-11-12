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
| 5 | BARs (BAR0 64 KB, BAR2 4 MB, BAR4 1 MB), iATU MEM window, BAR0 backplane window | chipcommon `0x10224383`: chip 4383 rev 2 |
| 6 | Walk the AXI enumeration ROM | 20 cores: chipcommon, PCIe2, **ARM Cortex-R4** (`0x18030000`), two 802.11 MACs, PMU, GCI… |
| 7 | Halt the CR4, firmware into its TCM (`0x6E0000`, 2.1 MB in 17 banks), NVRAM + length token, random-seed rTLV, reset vector, run | **firmware up in 680 ms: `pciedev_shared` at `0x8331d0`, protocol 7, no trap** |

The firmware files come from the phone's own vendor partition (`scripts/ext4cat.py` reads them out of a `vendor.img` dump) and
are copied to `/Users/akita/.akita/wifi/` on the phone; they are never redistributed.

Next: the msgbuf protocol (host-memory rings, doorbells), the CLM blob, the MAC address, a scan and WPA2 association; finally an
`IOEthernetController` for macOS.
