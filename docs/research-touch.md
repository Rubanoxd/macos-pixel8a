# Pixel 8a touchscreen on macOS — research and implementation notes

Research: 2026-11-11. Implemented the same day in `platform-akita/drivers/Acpm/AkitaTouch.inc` (working, with gestures).

## Summary

- A single chip (Goodix, Berlin family, "nottingham") on an SPI bus. Its firmware lives in the chip's own flash: after
  power-on and reset release it reports touches **without loading anything**.
- **Google publishes the driver** (GPL): `google-modules/touch/goodix_touch`, branch `android-gs-akita-5.15-android14-qpr3`.
  It documents the exact protocol: SPI framing, addresses, the per-touch structure and the acknowledge.
- **macOS side:** `NativeRecoveryInput` already creates an absolute HID tablet (the viewer's). A touch becomes "move + click"
  and dragging becomes "drag", with coordinates rotated to the landscape desktop.
- **Bus negotiator:** on Android the bus is shared with the AOC (low-power coprocessor) and negotiated through AOC firmware
  messages (`tbn,mode = <2>`). We do not start the AOC, and the bus turned out to be free for the AP.

## Hardware (device tree: `zuma-common-touch.dtsi`, `zuma-akita-touch.dtsi`)

| Piece | Data |
|---|---|
| Touch controller | `goodix,nottingham`, SPI CS0, `spi-max-frequency` 10 MHz, mode 0, 8 bits |
| SPI bus | `spi_20: spi@111D0000` (`samsung,exynos-spi`, FIFO 64, IRQ SPI 0x198) |
| USI (serial block mode) | `sysreg_hsi0@11020000`, offset `0x101c` = 2 → SPI mode; clocks in CMU_HSI0 `0x11000000` |
| Touch interrupt | `gpn0-0` (TS1_INT_L, *alive* bank `pinctrl@15060000`), falling edge |
| Reset | `gpp21-2` (TS1_RESET_L, `pinctrl@10C40000` + 0x40), active low |
| Rails | DVDD 1.8 V = `m_ldo25`; AVDD 3.0 V = `m_ldo4`, from the main PMIC (S2MPG14) through ACPM. We use ACPM channel 2: LDO25M reg `0x43 = 0xAC` (1.8 V), LDO4M reg `0x2E = 0xB0` (3.0 V) |
| Panel | 1080×2400 portrait (`panel-max-x 1079`, `max-y 2399`), pressure and size 0..255 |

## Protocol (Google's driver, akita branch)

**SPI** (`goodix_brl_spi.c`):
- **Read:** send `F1 a3 a2 a1 a0 FF FF FF` (4-byte big-endian address and 3 padding bytes); data starts at byte 8.
- **Write:** send `F0 a3 a2 a1 a0` followed by the data.
- Mode 0, 8 bits per word.

**Power-on** (`brl_power_on`): iovdd (1.8 V), wait 3 ms; avdd (3.0 V), wait 15 ms; release reset, wait 4 ms;
check (`brl_dev_confirm`): write 8 × `0xAA` to `0x10000`, read back and compare; wait 100 ms.

**Chip info** (`brl_get_ic_info`): at `0x10070` a u16 length (< 1024) and a packed block with a checksum, giving
`cmd_addr`, `touch_data_addr` and `point_struct_len`. On our 8a: `cmd_addr 0x10174`, `touch_data_addr 0x10308`, point length 14.
Firmware version at `0x10014`.

**Reading a touch** (when the interrupt line drops):
1. Read from `touch_data_addr`.
2. **8-byte header:** byte 0 type in the high nibble (`0x80` touch, `0x40` request, `0x20` gesture, `0x02` status);
   byte 2 finger count in the low nibble; bytes 6–7 checksum.
3. **Each finger, from byte 8,** is `point_struct_len` bytes: `id = data[0] >> 4`; `x` u16 LE at +2, `y` at +4, `w` at +6.
4. **Acknowledge:** write **one byte `0x00` to `touch_data_addr`**.

**Commands:** 16 bytes written to `cmd_addr` with a checksum; the chip answers with an ack byte (`0x80` = OK).
Sleep `{00 00 05 C4 01 CA 00}`, wake `{00 00 04 A7 AB 00}`, report rate command `0x9D`.

**Firmware:** runs from the chip's flash. **Never write the flash:** never write `0x12400`, `0x23800`, or `0x55` to `0x10000`
(that starts its ISP), and never send firmware updates.

## Implementation in the port

- `AkitaTouch.inc` is included by `AkitaAcpm.cpp` (it needs ACPM for the PMIC): rails, pins, USI/SPI master, reset,
  chip confirm, chip info, then `liveLoop` polls the interrupt GPIO and reads, acknowledges and delivers touches.
- Enabled with the boot-arg `akita-touch=1`; started 20 s after the GPU add-on loads.
- Gesture state machine (`kIdle/kPending/kDrag/kHeld/kScroll`): tap = left click, drag = left drag, long press = right click,
  two fingers = scroll wheel.
- Delivery: `akita_touch_set_sink()` registers `NativeRecoveryInput::touchSink` (codes 0 up, 1 left down, 2 right click,
  3 wheel, 4 move). Landscape mapping `x = py * W / 2400`, `y = (1079 - px) * H / 1080`.

## Sources

- [goodix_touch](https://android.googlesource.com/kernel/google-modules/touch/goodix_touch/), branch
  `android-gs-akita-5.15-android14-qpr3`: `goodix_brl_spi.c`, `goodix_brl_hw.c`, `goodix_ts_core.c`, `goodix_ts_core.h`
- `google-modules/touch/common`, akita branch: `touch_bus_negotiator.c`
