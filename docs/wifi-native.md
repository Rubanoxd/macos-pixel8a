# Native Wi-Fi in macOS: what macOS 26 expects (notes)

Our Wi-Fi driver (`platform-akita/drivers/Acpm/AkitaPcie.inc`) is an ifnet of its own, `en1`. It works: WPA2, DHCP, internet.
To make macOS treat it as Wi-Fi:

- **System Settings / SystemConfiguration.** An interface is listed when an `IONetworkInterface` (with `BSD Name`,
  `IOInterfaceType`, `IOInterfaceUnit`) sits under a controller with `IOMACAddress`. It is "Wi-Fi" when that controller conforms to
  `IO80211Controller`. `AkitaNetIf.inc` publishes those classes; IONetworkingFamily and IO80211Family are not in this kernel.
  The service shows **Connected** once `en1` answers `SIOCGIFMEDIA` and posts `KEV_DL_LINK_ON` (fullos270).
- **Wi-Fi type (boot-arg `akita-netif-wifi=1`).** System Settings shows a Wi-Fi port, but airportd finds no IO80211 driver
  behind it. Ethernet stays the default.
  - Correction (fullos277): the missing menu bar in fullos271/276 was not the Wi-Fi. It was System Settings, restored at
    login and stuck as the active app; with Finder in front the menu bar is there.
- **How airportd talks to a Wi-Fi driver (IO80211.framework, 26.5).**
  - `Apple80211RawGet` uses `ioctl(SIOCGA80211 = 0xC02869C9)` on the BSD interface, and `Apple80211RawSet` uses
    `SIOCSA80211 = 0x802869C8`, with the classic 40-byte `apple80211req`. These would reach `en1`'s ioctl handler.
  - airportd also opens `IO80211APIUserClient` (IOServiceOpen and IOConnectCall*), and it waits on driver states such as
    `kIO80211ReturnInitializing`.
  - As with third-party drivers since macOS 14, a Wi-Fi menu likely needs IO80211Family's Skywalk-based interface classes,
    not just the ioctls.
- **Apple's own Broadcom stack is in the BootKC.** It holds `com.apple.iokit.IO80211Family`, `IOSkywalkFamily`,
  `com.apple.driver.AppleBCMWLANCore` and `AppleBCMWLANBusInterfacePCIe`. The phone's chip is a Broadcom too (BCM4383), but that
  driver expects Apple's PCIe stack (IOPCIFamily) and Apple's firmware and NVRAM.

Next options, from cheapest:
1. Answer the classic SIOCGA80211 requests in `en1` (SSID, BSSID, RSSI, channel, state, power, card capabilities). Then see how
   far airportd gets.
2. Link IO80211Family from the BootKC and subclass its controller and interface (the AirportItlwm approach). This needs the
   26.5 class ABIs.
3. Bring up AppleBCMWLANCore on the BCM4383: it needs an IOPCIDevice shim for the Exynos PCIe link and firmware compatibility.

## Experiment, fullos275-276 (boot-arg `akita-netif-wifi=1`)

- How airportd finds a Wi-Fi interface (IO80211.framework `_getIfListCopy`, `_Apple80211FindService`, `_isInfraInterface`):
  - the BSD interface's `SIOCGIFMEDIA` must say IEEE 802.11;
  - an IOKit service must match `IOPropertyMatch {IOInterfaceName: "en1"}`, the full BSD name;
  - that service needs `IO80211InterfaceRole = "Infrastructure"`.
  `en1` does all three in Wi-Fi mode.
- What happens then:
  - `networksetup -getairportpower en1` answers "Off (forced)";
  - configd's only Apple80211 ioctl is GET type 12 (card capabilities, 21 bytes);
  - airportd sends none.
- airportd's power and state come through `IO80211APIUserClient`, which our IOKit classes do not provide. That user client is the
  next thing to reverse (IO80211.framework `Apple80211BindToInterface*`) before the ioctls are worth answering.

## fullos283-286: macOS's own Wi-Fi UI comes up (boot-arg `akita-netif-wifi=1`)

Three more things were needed:

- **The Wi-Fi subfamily.** IO80211.framework `_getIfListCopy` keeps an interface only if `SIOCGIFTYPE` says type 6,
  subfamily 3 (Wi-Fi), and its media is 802.11 (0x80) with no error. `en1` is now created with `IFNET_SUBFAMILY_WIFI`, and
  the media ioctl no longer fails with EFAULT.
- **The classic Apple80211 ioctls.** When `IOServiceOpen` fails, `Apple80211BindToInterfaceWithService` falls back to the
  ioctl path. Its sanity check is GET type 12 (card capabilities). `en1` now answers:
  - GET 12 card capabilities, 43 driver version, 19 power, 13 state, 15 op mode;
  - GET 1 SSID, 9 BSSID (from the configured network's scan entry), 4 channel, 16 RSSI, 17 noise, 14 PHY mode, 8 rate,
    51 country, 27 supported channels;
  - SET 19 power and 5 powersave.
- **Apple80211 kernel events.** Class `KEV_IEEE80211_CLASS`, subclass 1, codes 4/2/3 (link, SSID, BSSID) are posted when the
  link changes, and once more on the first query, so airportd asks again.

Result:
- CoreWLAN lists `en1` (power on, service active, RSSI, channel, rate).
- The menu bar shows the Wi-Fi icon (connected).
- System Settings has a Wi-Fi pane with its power switch.

Still to do:
- scanning (SET 10 scan request, GET 11 scan results; airportd also asks 103 and 207);
- the pane's "Connected" state.

## fullos288: scanning works

- **Scan request (SET 10).** Accepted. It starts a firmware escan from a thread call; when the firmware reports the scan done,
  `en1` posts `SCAN_DONE` (Apple80211 event 10).
- **Scan results (GET 11).** One network per call, in the 2272-byte (0x8E0) layout IO80211's `_addScanResultToList` reads:

  | Offset | Field |
  |---|---|
  | 0x08 / 0x0C | channel number and flags |
  | 0x12 / 0x14 / 0x16 | noise, SNR, RSSI |
  | 0x18 / 0x1A | beacon interval, capability |
  | 0x1C | BSSID |
  | 0x22 / 0x24 | rate count and rates |
  | 0x60 / 0x61 | SSID length and SSID |
  | 0x84 | age |
  | 0x88 | flags |
  | 0x8A / 0x8C | IE length and IEs |
  | 0x8D0 | timestamp |

  `EIO` ends the list (Apple80211Scan stops on errno 5).
- **Result.** CoreWLAN's scan returns the networks around, and System Settings lists them with their lock and signal icons.
  The driver keeps the networks by BSSID, with up to 256 bytes of IEs each; they are never logged.
- **Next.** GET 103 (the current network, same layout) for the pane's "Connected"; GET 207.
