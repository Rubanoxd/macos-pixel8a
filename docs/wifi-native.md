# Native Wi-Fi in macOS: what macOS 26 expects (notes)

Our Wi-Fi driver (`platform-akita/drivers/Acpm/AkitaPcie.inc`) is an ifnet of its own, `en1`. It works: WPA2, DHCP, internet.
To make macOS treat it as Wi-Fi:

- **System Settings / SystemConfiguration.** An interface is listed when an `IONetworkInterface` (with `BSD Name`,
  `IOInterfaceType`, `IOInterfaceUnit`) sits under a controller with `IOMACAddress`. It is "Wi-Fi" when that controller conforms to
  `IO80211Controller`. `AkitaNetIf.inc` publishes those classes; IONetworkingFamily and IO80211Family are not in this kernel.
  The service shows **Connected** once `en1` answers `SIOCGIFMEDIA` and posts `KEV_DL_LINK_ON` (fullos270).
- **Wi-Fi type (boot-arg `akita-netif-wifi=1`).** System Settings shows a Wi-Fi port. But airportd, ControlCenter and System
  Settings (CoreWLAN) then find no IO80211 driver behind it, and the menu bar went missing (fullos271). So Ethernet is the default.
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
