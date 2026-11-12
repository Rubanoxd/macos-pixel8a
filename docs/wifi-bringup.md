# Wi-Fi bring-up

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
| 8 | msgbuf: five common rings in host memory (indices in the chip's TCM), ioctl/event buffers posted, `WLC_GET_VAR cur_etheraddr` | **the firmware answers with the MAC address** (rings in the low DRAM bank, see below) |
| 9, 20 | DMA loopback; the firmware's own console (`hnd_cons_t`, read through BAR0) | the console showed our messages arriving as zeros |
| 21 | CLM blob (`clmload`, 1400-byte chunks), `event_msgs`, `WLC_UP`, `escan` (params v3; v1 and v4 refused with BCME_VERSION), `ESCAN_RESULT` events | **scan works: 26 results on 2.4 and 5 GHz in 8 s** |
| 22 | WPA2-PSK join with the firmware's supplicant (`sup_wpa`, `WLC_SET_WSEC_PMK`, `WLC_SET_SSID`); network from a file the user writes | written, waiting for credentials |
| 23 | RX buffers on the rxpost ring, a TX flow ring (`FLOW_RING_CREATE`), polling thread, BSD interface with Ethernet framing | **`en1` appears in macOS; the firmware accepts the flow ring** |
| boot | boot-arg `akita-wifi=1`: every stage runs by itself a minute after boot; a stable MAC derived from the platform UUID; `rc.server` asks for DHCP on `en1` | **Wi-Fi comes up on its own at every boot** |

### Connecting to a network

Write the network on the phone (your own network; the passphrase never goes to the log) and reboot, or run stage 22:

```sh
mkdir -p /Users/akita/.akita/wifi
printf 'ssid=YOUR-NETWORK\npsk=YOUR-PASSPHRASE\n' > /Users/akita/.akita/wifi/network.conf
chmod 600 /Users/akita/.akita/wifi/network.conf
```

**The DMA finding.** The chip's writes into macOS memory worked from the start, but its *reads* of macOS memory returned
zeros: every message was rejected (`RING_STATUS` BADOPTION) and the firmware console printed `unknown message: 8 / 00 00 00 00…`.
With the rings placed in a free gap of the phone's low DRAM bank (`0xD0000000`, outside macOS's RAM) the same messages work.
Most likely the HSI2 S2MPU (a stage-2 DMA filter the bootloader configures; its registers are secure and fault when read) only
lets the PCIe channel read some regions. The driver will keep its DMA buffers in a pool in the low bank (~330 MB free there).

The firmware files come from the phone's own vendor partition (`scripts/ext4cat.py` reads them out of a `vendor.img` dump) and
are copied to `/Users/akita/.akita/wifi/` on the phone; they are never redistributed.

### Internet over Wi-Fi

The phone joins a WPA2 network at boot, gets a DHCP lease, and reaches the internet: DNS resolves and
`curl https://www.apple.com` answers 200 in 0.17 s. Getting there needed four more fixes.

- **The firmware never hands ARP to the host.** ARP requests do go out: the Mac on the same network learned the phone's MAC from
  them. But in about 2 minutes 0 ARP frames came in, against 69 sent. That includes the router's replies and other hosts'
  broadcast requests. Turning ARP/ND offload off (`arp_ol`, `arpoe`, `ndoe` = 0) changed nothing.
  The driver now does **ARP gleaning**. Any IPv4 frame from a neighbour on our subnet gives its MAC, for example the router's
  DHCP answers or the Mac's hello. The driver hands the stack a synthetic ARP reply for it, at most once a minute per address.
  Internet traffic arrives from the router's MAC carrying a remote source IP, so a frame like that refreshes the entry that owns
  the MAC. The stack's own ARP requests, sent to refresh an entry, are answered from the same table. Without both, the
  router's entry went stale after a few minutes: no route to host, and DNS timed out.
- **The neighbours' ARP questions are swallowed too**, so the router's entry for the phone expired and it stopped answering,
  while the phone's frames still reached the Mac. Every 20 s the driver sends each neighbour heard in the last 10 minutes a
  unicast ARP request for its own address. The target of a request always learns the sender's MAC.
  With all of this (fullos231), a 12-minute check every 80 s passed every round: Wi-Fi shell, default route, HTTPS 200 and the
  USB shell, all at once.
- Stages 28/29 (`sysctl kern.akita_acpm=4000028` / `4000029`) turn the firmware's ARP offload on the Android way or off
  again, for testing. "The Android way" means agent and peer auto-reply with our address in `arp_hostip`. The Pixel firmware
  carries `apf`, `l2_filter`, `pkt_filter` and an undocumented `hwfilter`. Which one drops ARP is still open.
- **Short frames are padded** to the 60-byte Ethernet minimum.
- **Something takes `en1` down a while after the lease**, and the address goes with it. The kernel now logs which process does
  it, and a boot-time loop brings `en1` back up and asks for DHCP again.
- **The USB link to the Mac dies** once `en1` has a lease. macOS makes `en1` the primary interface and moves the 169.254/16
  route to it, so replies to the Mac's USB link-local address leave through Wi-Fi. The boot-time loop moves the route back to
  `en0` (`route change -net 169.254.0.0/16 -interface en0`). USB and Wi-Fi shells then work at the same time.

`pcie: traffic …` lines in the kernel log count frames by kind every 20 s. The kinds are broadcast, multicast, to us and ARP,
plus TX completions with an error status.

### Control channel over Wi-Fi (for when the USB-C port holds a dongle)

The same idea as the USB gadget's channel (UDP 5140/5141), built and parsed in the driver, beside the BSD stack, so it works
while userland is wedged:

- The Mac sends `H` (hello) to UDP 5141, by broadcast or unicast. It becomes the **peer**, and the driver streams the kernel's
  retained log to it, from the start, by unicast to the port the hello came from.
- `L` replays the log. `FASTBOOT` resets into fastboot. Only the peer may send these.
- The root shell ports (TCP 2323-2325) answer only the peer over Wi-Fi. Frames from anyone else are dropped in the driver.

On the Mac:

```sh
python3 scripts/akita-live.py wlog          # hello by broadcast, live log, writes the phone's IP to scratch/live/phone-wifi-ip
AKHOST=$(cat scratch/live/phone-wifi-ip) scratch/akx 'uname -a'
python3 scripts/akita-live.py wfastboot     # back to fastboot over the network
```
