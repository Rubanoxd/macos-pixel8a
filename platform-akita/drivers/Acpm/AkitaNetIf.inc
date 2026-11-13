// akita (Generated-by: LLM, local only): the Wi-Fi interface (en1, an ifnet of our own in AkitaPcie.inc) described in IOKit
// the way IONetworkingFamily would, so SystemConfiguration lists it (System Settings > Network, scutil, networksetup).
// IONetworkingFamily is not in this kernel, so its class names are free: a controller carrying the MAC address and, as its
// child, an interface carrying the BSD name. Nothing here touches the data path; the ifnet stays as it is.
// Sources: SystemConfiguration's SCNetworkInterface.c (processNetworkInterface: IOInterfaceType, IOInterfaceUnit,
// IOBuiltin, IOPrimaryInterface, BSD Name; the controller's IOMACAddress), IONetworkInterface.h / IONetworkController.h keys.

class IONetworkController : public IOService {
  OSDeclareDefaultStructors(IONetworkController);
};
OSDefineMetaClassAndStructors(IONetworkController, IOService)

class IOEthernetController : public IONetworkController {
  OSDeclareDefaultStructors(IOEthernetController);
};
OSDefineMetaClassAndStructors(IOEthernetController, IONetworkController)

class IONetworkInterface : public IOService {
  OSDeclareDefaultStructors(IONetworkInterface);
};
OSDefineMetaClassAndStructors(IONetworkInterface, IOService)

class IOEthernetInterface : public IONetworkInterface {
  OSDeclareDefaultStructors(IOEthernetInterface);
};
OSDefineMetaClassAndStructors(IOEthernetInterface, IONetworkInterface)

namespace netif {

IOEthernetController *gController = nullptr;
IOEthernetInterface *gInterface = nullptr;

void setNumber(IOService *s, const char *key, unsigned long long v, unsigned bits) {
  OSNumber *n = OSNumber::withNumber(v, bits);
  if (n) { s->setProperty(key, n); n->release(); }
}

// en1 as SystemConfiguration expects a built-in Ethernet-type interface (unit 1, not primary)
void publish(const uint8_t mac[6], unsigned unit) {
  if (gInterface) return;
  IOService *platform = IOService::getPlatform();
  if (!platform) return;
  gController = new IOEthernetController;
  if (!gController || !gController->init() || !gController->attach(platform)) {
    printf("netif: could not make the controller\n");
    if (gController) { gController->release(); gController = nullptr; }
    return;
  }
  gController->setName("wlan");
  OSData *addr = OSData::withBytes(mac, 6);
  if (addr) { gController->setProperty("IOMACAddress", addr); addr->release(); }
  gController->setProperty("IOVendor", "Broadcom");
  gController->setProperty("IOModel", "BCM4383 (Pixel 8a Wi-Fi)");
  gController->setProperty("IOBuiltin", true);
  setNumber(gController, "IOLinkStatus", 3, 32);   // kIONetworkLinkValid | kIONetworkLinkActive
  setNumber(gController, "IOLinkSpeed", 600000000ull, 64);
  setNumber(gController, "IOMaxPacketSize", 1518, 32);
  gController->registerService();

  gInterface = new IOEthernetInterface;
  if (!gInterface || !gInterface->init() || !gInterface->attach(gController)) {
    printf("netif: could not make the interface\n");
    if (gInterface) { gInterface->release(); gInterface = nullptr; }
    return;
  }
  char bsd[8];
  snprintf(bsd, sizeof bsd, "en%u", unit);
  gInterface->setName(bsd);
  gInterface->setProperty("BSD Name", bsd);
  gInterface->setProperty("IOInterfaceName", "en");
  gInterface->setProperty("IOInterfaceNamePrefix", "en");
  setNumber(gInterface, "IOInterfaceUnit", unit, 32);
  setNumber(gInterface, "IOInterfaceType", 6, 8);   // IFT_ETHER
  setNumber(gInterface, "IOMaxTransferUnit", 1500, 32);
  setNumber(gInterface, "IOMediaAddressLength", 6, 32);
  setNumber(gInterface, "IOMediaHeaderLength", 14, 32);
  gInterface->setProperty("IOBuiltin", true);
  gInterface->setProperty("IOPrimaryInterface", false);
  gInterface->setProperty("IOLocation", "");
  gInterface->registerService();
  printf("netif: en%u published in IOKit (IOEthernetInterface under IOEthernetController)\n", unit);
}

}  // namespace netif
