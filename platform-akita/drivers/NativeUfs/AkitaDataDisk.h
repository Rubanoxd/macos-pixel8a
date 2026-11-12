// SPDX-License-Identifier: BSD-2-Clause-Patent
// akita (Generated-by: LLM): a plain writable disk over a fixed window of the userdata partition, for a persistent
// APFS Data volume (and swap). Off unless the boot argument akita-data-disk is given:
//   akita-data-disk=1   publish the window read-only (probe: geometry in the log, nothing written)
//   akita-data-disk=2   publish it read-write (opens the store write window; every write is bounded by hostWrite4k)
// The window starts 32 GiB into userdata and is 64 GiB long. It is refused unless it lies clear of the flashed root image
// (plus a 4 MiB guard) and of the reserve at the end of userdata (the diagnostic log and its export region). The disk is
// published after the root windows, so it can never be taken for the root volume.
// Included by NativeUfs.cpp after NativeUfsBlock; uses hostRead4k / hostWrite4k / hostStoreOpen / hostSyncCache.

extern "C" {
volatile uint32_t akita_data_disk_mode = 0, akita_data_disk_published = 0;
volatile uint64_t akita_data_disk_first = 0, akita_data_disk_blocks = 0, akita_data_disk_reads = 0, akita_data_disk_writes = 0,
                  akita_data_disk_syncs = 0, akita_data_disk_failures = 0;
const char *volatile akita_data_disk_refusal = "";
}

class AkitaDataDisk : public IOBlockStorageDevice {
  OSDeclareDefaultStructors (AkitaDataDisk)
  uint64_t first = 0;         // absolute 4096-byte LBA of the window on LUN 0
  uint64_t blocks = 0;        // 4096-byte blocks
  bool writable = false;
  bool online = false;
  uint8_t *bounce = nullptr;
  IOLock *lock = nullptr;
public:
  bool configure (uint64_t firstLba, uint64_t count, bool rw);
  bool start (IOService *provider) override;
  void free () override;
  IOReturn doEjectMedia () override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia (UInt64) override { return kIOReturnUnsupported; }
  UInt32 doGetFormatCapacities (UInt64 *, UInt32) const override { return 0; }
  char *getVendorString () override { return const_cast<char *> ("Native"); }
  char *getProductString () override { return const_cast<char *> ("AkitaData"); }
  char *getRevisionString () override { return const_cast<char *> ("1.0"); }
  char *getAdditionalDeviceInfoString () override { return const_cast<char *> ("userdata window for the persistent Data volume"); }
  IOReturn reportBlockSize (UInt64 *value) override { *value = 4096; return kIOReturnSuccess; }
  IOReturn reportEjectability (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock (UInt64 *value) override { *value = blocks - 1; return kIOReturnSuccess; }
  IOReturn reportMediaState (bool *present, bool *changed) override {
    *present = online; if (changed) *changed = false; return kIOReturnSuccess;
  }
  IOReturn reportRemovability (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection (bool *value) override { *value = !writable; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache () override {
    if (!online) return kIOReturnNotReady;
    if (!writable) return kIOReturnSuccess;
    IOReturn r = hostSyncCache ();
    if (r == kIOReturnSuccess) __atomic_fetch_add (&akita_data_disk_syncs, 1, __ATOMIC_RELAXED);
    else __atomic_fetch_add (&akita_data_disk_failures, 1, __ATOMIC_RELAXED);
    return r;
  }
  IOReturn doSynchronize (UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache (); }
  IOReturn getWriteCacheState (bool *value) override { *value = writable; return kIOReturnSuccess; }
  IOReturn setWriteCacheState (bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite (IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *, IOStorageCompletion *) override;
};
OSDefineMetaClassAndStructors (AkitaDataDisk, IOBlockStorageDevice)

bool AkitaDataDisk::configure (uint64_t firstLba, uint64_t count, bool rw)
{
  first = firstLba;
  blocks = count;
  writable = rw;
  OSDictionary *protocol = OSDictionary::withCapacity (2);
  OSString *interconnect = OSString::withCString (kIOPropertyPhysicalInterconnectTypeVirtual);
  OSString *location = OSString::withCString (kIOPropertyInternalKey);
  bool described = protocol && interconnect && location &&
    protocol->setObject (kIOPropertyPhysicalInterconnectTypeKey, interconnect) &&
    protocol->setObject (kIOPropertyPhysicalInterconnectLocationKey, location) &&
    setProperty (kIOPropertyProtocolCharacteristicsKey, protocol);
  OSSafeReleaseNULL (location);
  OSSafeReleaseNULL (interconnect);
  OSSafeReleaseNULL (protocol);
  if (!described) return false;
  lock = IOLockAlloc ();
  bounce = static_cast<uint8_t *> (IOMalloc (kDataMax));
  return lock && bounce;
}

bool AkitaDataDisk::start (IOService *provider)
{
  if (!bounce || !lock || !IOBlockStorageDevice::start (provider)) return false;
  online = true;
  registerService ();
  return true;
}

void AkitaDataDisk::free ()
{
  if (bounce) { IOFree (bounce, kDataMax); bounce = nullptr; }
  if (lock) { IOLockFree (lock); lock = nullptr; }
  IOBlockStorageDevice::free ();
}

IOReturn AkitaDataDisk::doAsyncReadWrite (IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                          IOStorageAttributes *, IOStorageCompletion *completion)
{
  if (!buffer || !completion || !completion->action || !count || block >= blocks || count > blocks - block ||
      buffer->getLength () < count * 4096) return kIOReturnBadArgument;
  bool write = buffer->getDirection () == kIODirectionOut;
  if (buffer->getDirection () != kIODirectionIn && !write) return kIOReturnBadArgument;
  if (write && !writable) return kIOReturnNotWritable;
  if (!online) return kIOReturnNotReady;
  IOReturn result = kIOReturnSuccess;
  uint64_t done = 0;   // bytes
  IOLockLock (lock);
  while (done < count * 4096) {
    uint64_t at = block + done / 4096;
    uint32_t chunk = static_cast<uint32_t> (count - done / 4096);
    if (chunk > kDataMax / 4096) chunk = kDataMax / 4096;
    uint64_t bytes = chunk * 4096ULL;
    if (write) {
      if (buffer->readBytes (done, bounce, bytes) != bytes) { result = kIOReturnUnderrun; break; }
      result = hostWrite4k (0, first + at, chunk, bounce);
    } else {
      result = hostRead4k (0, first + at, chunk, bounce);
      if (result == kIOReturnSuccess && buffer->writeBytes (done, bounce, bytes) != bytes) result = kIOReturnUnderrun;
    }
    if (result != kIOReturnSuccess) break;
    done += bytes;
  }
  IOLockUnlock (lock);
  if (result != kIOReturnSuccess) {
    __atomic_fetch_add (&akita_data_disk_failures, 1, __ATOMIC_RELAXED);
    IOLog ("AkitaDataDisk: %s at block %llu failed 0x%x\n", write ? "write" : "read", block + done / 4096, result);
  } else {
    __atomic_fetch_add (write ? &akita_data_disk_writes : &akita_data_disk_reads, 1, __ATOMIC_RELAXED);
  }
  completion->action (completion->target, completion->parameter, result, done);
  return kIOReturnSuccess;
}

namespace {
constexpr uint64_t kAkitaDataOffsetBlocks = (32ULL << 30) / 4096;   // 32 GiB into userdata
constexpr uint64_t kAkitaDataBlocks = (64ULL << 30) / 4096;         // 64 GiB
IOService *gAkitaDataProvider = nullptr;
uint64_t gAkitaDataT[4];   // uptime seconds: start called, thread running, wait done, published
uint64_t akitaUptime (void) { clock_sec_t sec = 0; clock_usec_t usec = 0; clock_get_system_microtime (&sec, &usec); return sec; }
unsigned gAkitaDataWindows = 0;

void akitaDataThread (void *, wait_result_t)
{
  // After the root windows: the disk is never a root candidate.
  gAkitaDataT[1] = akitaUptime ();
  // fullos185: waiting for every root window delayed the disk past rc.server. The root is chosen by its volume UUID, so the
  // first published window is enough; the extra windows are plain disks anyway.
  for (unsigned waited = 0; !native_ufs_blocks_published && waited < 60; ++waited) IOSleep (500);
  IOSleep (1000);
  gAkitaDataT[2] = akitaUptime ();
  const char *why = "";
  const unsigned mode = akita_data_disk_mode;
  const StoreGeometry g = gStoreGeometry;
  const uint64_t imageEnd = g.first + (g.imageBytes + 4095) / 4096 + 1024;   // 4 MiB guard
  const uint64_t firstLba = g.first + kAkitaDataOffsetBlocks;
  const uint64_t reserveFirst = g.last + 1 - reserveBlocks ();
  if (!g.valid) why = "no root layout";
  else if (firstLba < imageEnd) why = "overlaps the root image";
  else if (firstLba + kAkitaDataBlocks > reserveFirst) why = "overlaps the log reserve";
  else if (mode == 2 && !hostStoreOpen (firstLba, kAkitaDataBlocks)) why = "write window refused";
  IOLog ("AkitaDataDisk: mode=%u userdata=%llu..%llu image=%llu bytes reserve@%llu window=%llu+%llu %s\n", mode,
         g.first, g.last, g.imageBytes, reserveFirst, firstLba, kAkitaDataBlocks, why[0] ? why : "ok");
  akita_data_disk_first = firstLba;
  akita_data_disk_blocks = kAkitaDataBlocks;
  if (why[0]) { akita_data_disk_refusal = why; gAkitaDataProvider->release (); return; }
  AkitaDataDisk *disk = OSTypeAlloc (AkitaDataDisk);
  if (!disk || !disk->init (static_cast<OSDictionary *> (nullptr)) || !disk->configure (firstLba, kAkitaDataBlocks, mode == 2) ||
      !disk->attach (gAkitaDataProvider)) {
    akita_data_disk_refusal = "device setup failed";
  } else if (!disk->start (gAkitaDataProvider)) {
    disk->detach (gAkitaDataProvider);
    akita_data_disk_refusal = "device start failed";
  } else {
    akita_data_disk_published = 1;
    gAkitaDataT[3] = akitaUptime ();
    char t[96];
    snprintf (t, sizeof (t), "start %llu thread %llu waited %llu published %llu", gAkitaDataT[0], gAkitaDataT[1], gAkitaDataT[2], gAkitaDataT[3]);
    disk->setProperty ("AkitaTimeline", t);
    IOLog ("AkitaDataDisk: published %s, %llu GiB\n", mode == 2 ? "read-write" : "read-only", (kAkitaDataBlocks * 4096) >> 30);
  }
  OSSafeReleaseNULL (disk);
  gAkitaDataProvider->release ();
}
}

static void akitaDataStart (IOService *provider, unsigned windows)
{
  uint32_t mode = 0;
  if (!PE_parse_boot_argn ("akita-data-disk", &mode, sizeof (mode)) || (mode != 1 && mode != 2)) return;
  akita_data_disk_mode = mode;
  gAkitaDataT[0] = akitaUptime ();
  gAkitaDataWindows = windows;
  gAkitaDataProvider = provider;
  provider->retain ();
  thread_t thread = nullptr;
  if (kernel_thread_start (akitaDataThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate (thread);
  else { provider->release (); akita_data_disk_refusal = "no thread"; }
}
