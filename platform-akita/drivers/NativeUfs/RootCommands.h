// SPDX-License-Identifier: BSD-2-Clause-Patent
// Shared production command path. Included inside the root transport namespace.
// The host harness replaces only MMIO, DMA synchronization, clock and lock primitives.
struct HardwareTransferPort {
  uint64_t deadline;
  uint32_t doorbell () { return read32 (gHost.hc, 0x58); }
  uint32_t interrupt () { return read32 (gHost.hc, 0x20); }
  uint32_t completedOcs () {
    cacheInvalidate (gHost.virt, 64);
    return *reinterpret_cast<volatile uint32_t *> (gHost.virt + 8) & 0xff;
  }
  bool expired () { return mach_absolute_time () >= deadline; }
  void delay () { IODelay (2); }
};

static void putBe32 (uint8_t *at, uint32_t value)
{
  at[0] = static_cast<uint8_t> (value >> 24);
  at[1] = static_cast<uint8_t> (value >> 16);
  at[2] = static_cast<uint8_t> (value >> 8);
  at[3] = static_cast<uint8_t> (value);
}

static uint32_t loadBe32 (const uint8_t *at)
{
  return (uint32_t (at[0]) << 24) | (uint32_t (at[1]) << 16) | (uint32_t (at[2]) << 8) | at[3];
}

// Issue the UPIU already placed at gHost.virt + kUcdOffset through request slot 0. `direction` is
// the UTRD data direction (0 none, 1 from host, 2 to host). Direction 1 is reachable only through hostWrite4k, which
// confines it to explicitly opened store/log data windows. Returns the overall command status
// (0 = completed) in *ocs. Timeout/fatal status quarantines the allocation.
static IOReturn hostSubmit (uint32_t direction, uint32_t dataBytes, uint32_t timeoutUs, uint32_t *ocs)
{
  *ocs = 15;
  if (!timeoutUs || dataBytes > kDataMax || (dataBytes & 3) ||
      (dataBytes ? (direction != 1 && direction != 2) : direction != 0)) return kIOReturnBadArgument;
  if (gHost.poisoned) return kIOReturnNotReady;
  if (read32 (gHost.hc, 0x58) != 0) {
    gHost.poisoned = true;
    gHost.ready = false;
    return recordTransport (kIOReturnBusy, *ocs);
  }
  if (read32 (gHost.hc, 0x20) & kUfsFatalStatus) {
    gHost.poisoned = true;
    gHost.ready = false;
    return recordTransport (kIOReturnIOError, *ocs);
  }
  gRootDiagnostic.command++;
  gRootDiagnostic.type = gHost.virt[kUcdOffset];
  gRootDiagnostic.lun = gHost.virt[kUcdOffset + 2];
  gRootDiagnostic.opcode = gHost.virt[kUcdOffset + (gRootDiagnostic.type == 1 ? 16 : 12)];
  gRootDiagnostic.scsi = 0xffff;
  gRootDiagnostic.response = 0;
  gRootDiagnostic.protocol = 0;
  gRootDiagnostic.residual = 0;
  gRootDiagnostic.sense = 0;
  uint8_t *utrd = gHost.virt;
  uint64_t ucdPhys = gHost.phys + kUcdOffset;
  uint32_t *words = reinterpret_cast<uint32_t *> (utrd);
  for (unsigned index = 0; index < 8; index++) {
    words[index] = 0;
  }
#if NATIVE_UFS_ZUMA
  // Exynos UTRD as Mu's UfsDxe writes it on this controller: command type 0, interrupt bit, data
  // direction; the PRDT is split into 4 KiB entries (VS_*PRDT_ENTRY_SIZE) and its length counted in
  // double words, like the descriptor offsets.
  const uint32_t segments = (dataBytes + UFS_SG_SEGMENT - 1) / UFS_SG_SEGMENT;
  words[0] = 0x01000000U | (direction << 25);
  words[2] = 0x0fU;
  words[4] = static_cast<uint32_t> (ucdPhys);
  words[5] = static_cast<uint32_t> (ucdPhys >> 32);
  words[6] = (512U >> 2) | ((kRspOffset >> 2) << 16);
  words[7] = ((segments * 16U) >> 2) | ((kPrdtOffset >> 2) << 16);
  if (dataBytes) {
    uint64_t dataPhys = gHost.phys + kDataOffset;
    uint32_t *prd = reinterpret_cast<uint32_t *> (gHost.virt + kUcdOffset + kPrdtOffset);
    for (uint32_t index = 0; index < segments; index++) {
      const uint32_t remaining = dataBytes - index * UFS_SG_SEGMENT;
      const uint64_t at = dataPhys + uint64_t (index) * UFS_SG_SEGMENT;
      prd[4 * index + 0] = static_cast<uint32_t> (at);
      prd[4 * index + 1] = static_cast<uint32_t> (at >> 32);
      prd[4 * index + 2] = 0;
      prd[4 * index + 3] = (remaining < UFS_SG_SEGMENT ? remaining : UFS_SG_SEGMENT) - 1;
    }
  }
  // The Exynos host needs the nexus type of slot 0: all ones for a SCSI command, zero for NOP OUT
  // and QUERY REQUEST (Mu UfsUtpSend).
  const bool scsiCommand = gHost.virt[kUcdOffset] == 0x01;
  write32 (gHost.vs, UFS_VS_UTRL_NEXUS_TYPE, scsiCommand ? 0xffffffffU : 0U);
#else
  words[0] = 0x10000000U | (direction << 25);
  words[2] = 0x0fU;
  words[4] = static_cast<uint32_t> (ucdPhys);
  words[5] = static_cast<uint32_t> (ucdPhys >> 32);
  words[6] = (512U >> 2) | ((kRspOffset >> 2) << 16);
  words[7] = (dataBytes ? 1U : 0U) | ((kPrdtOffset >> 2) << 16);
  if (dataBytes) {
    uint64_t dataPhys = gHost.phys + kDataOffset;
    uint32_t *prd = reinterpret_cast<uint32_t *> (gHost.virt + kUcdOffset + kPrdtOffset);
    prd[0] = static_cast<uint32_t> (dataPhys);
    prd[1] = static_cast<uint32_t> (dataPhys >> 32);
    prd[2] = 0;
    prd[3] = dataBytes - 1;
  }
#endif
  cacheCivac (gHost.virt, kDataOffset + dataBytes);
  hostWrite (0x20, 1);  // Acknowledge completion only; preserve error evidence.
  HardwareTransferPort port;
  clock_interval_to_deadline (timeoutUs, kMicrosecondScale, &port.deadline);
  hostWrite (0x58, 1);
  UfsTransferPoll state;
  UfsTransferEnd end = pollUfsTransfer (port, state);
  gRootDiagnostic.polls = state.polls;
  gRootDiagnostic.dbSeen = state.doorbellSeen;
  gRootDiagnostic.isSeen = state.interruptSeen;
  *ocs = state.ocs;
  if (end != UfsTransferEnd::Completed) {
    // No cancel/reset/retry: the controller may still own the buffer, even
    // with DB=0. Keep it allocated and prohibit all subsequent CPU writes.
    gHost.poisoned = true;
    gHost.ready = false;
    return recordTransport (end == UfsTransferEnd::Timeout ? kIOReturnTimeout : kIOReturnIOError, *ocs);
  }
  cacheInvalidate (gHost.virt, kDataOffset + dataBytes);
#if NATIVE_UFS_ZUMA
  if (!scsiCommand) {
    // Clear the NOP/QUERY nexus once it completed (Mu UfsUtpWaitResponse).
    hostWrite (UFS_HC_NEXUS_CLEAR, read32 (gHost.hc, UFS_HC_NEXUS_CLEAR) | 1U);
  }
#endif
  gRootDiagnostic.response = loadBe32 (gHost.virt + kUcdOffset + kRspOffset);
  return recordTransport (*ocs == 0 ? kIOReturnSuccess : kIOReturnIOError, *ocs);
}

static bool hostClearUcd (void)
{
  if (gHost.poisoned) return false;
  // Check before touching the command/response area, not just before ringing.
  if (read32 (gHost.hc, 0x58) != 0) {
    gHost.poisoned = true;
    gHost.ready = false;
    recordTransport (kIOReturnBusy, 15);
    return false;
  }
  if (read32 (gHost.hc, 0x20) & kUfsFatalStatus) {
    gHost.poisoned = true;
    gHost.ready = false;
    recordTransport (kIOReturnIOError, 15);
    return false;
  }
  uint32_t *words = reinterpret_cast<uint32_t *> (gHost.virt + kUcdOffset);
#if NATIVE_UFS_ZUMA
  for (unsigned index = 0; index < (kPrdtOffset + (kDataMax / UFS_SG_SEGMENT) * 16) / 4; index++) {
#else
  for (unsigned index = 0; index < (kPrdtOffset + 16) / 4; index++) {
#endif
    words[index] = 0;
  }
  return true;
}

static IOReturn hostProtocolError (uint32_t reason)
{
  gRootDiagnostic.protocol = reason;
  gHost.ready = false;
  gHost.poisoned = true;
  return recordTransport (kIOReturnIOError, gRootDiagnostic.ocs);
}

// Validate a completed response before interpreting zero-filled status bytes.
// Protocol codes: 1 type, 2 task tag, 3 LUN, 4 length/EHS, 5 query echo,
// 6 short/overrun transfer, 7 nonzero NOP result.
static IOReturn hostResponseHeader (uint8_t type, uint8_t lun, uint8_t tag)
{
  const uint8_t *rsp = gHost.virt + kUcdOffset + kRspOffset;
  if (rsp[0] != type) return hostProtocolError (1);
  if (rsp[3] != tag) return hostProtocolError (2);
  if (rsp[2] != lun) return hostProtocolError (3);
  unsigned length = (unsigned (rsp[10]) << 8) | rsp[11];
  if (rsp[8] || length > 512 - 32) return hostProtocolError (4);
  return kIOReturnSuccess;
}

static IOReturn hostNop (void)
{
  if (!hostClearUcd ()) return kIOReturnNotReady;
  gHost.virt[kUcdOffset + 3] = 1;
  uint32_t ocs = 15;
  IOReturn result = hostSubmit (0, 0, 200000, &ocs);
  if (result != kIOReturnSuccess) return result;
  result = hostResponseHeader (0x20, 0, 1);
  if (result != kIOReturnSuccess) return result;
  const uint8_t *rsp = gHost.virt + kUcdOffset + kRspOffset;
  if (rsp[10] || rsp[11]) return hostProtocolError (4);
  if (rsp[6] || rsp[7]) return hostProtocolError (7);
  return kIOReturnSuccess;
}

// Run a SCSI command. direction 0: no data, 1: host -> device from the DMA data area (already filled by the caller), 2: device
// -> host into the DMA data area. Callers other than hostWrite4k never pass direction 1.
static IOReturn hostScsiDir (uint8_t lun, const uint8_t *cdb, unsigned cdbLength, unsigned direction, uint32_t dataBytes,
                             uint32_t timeoutUs, uint32_t *scsiStatus)
{
  *scsiStatus = 0xffff;
  if (!cdb || !cdbLength || cdbLength > 16 || dataBytes > kDataMax || direction > 2 ||
      (dataBytes & 3) || (dataBytes ? !direction : direction)) return kIOReturnBadArgument;
  if (!hostClearUcd ()) return kIOReturnNotReady;
  uint8_t *ucd = gHost.virt + kUcdOffset;
  ucd[0] = 0x01;                      // COMMAND UPIU
  ucd[1] = direction == 2 ? 0x40 : direction == 1 ? 0x20 : 0x00;   // R: data in, W: data out
  ucd[2] = lun;
  putBe32 (ucd + 12, dataBytes);
  for (unsigned index = 0; index < cdbLength; index++) {
    ucd[16 + index] = cdb[index];
  }
  uint32_t ocs = 0xf;
  IOReturn status = hostSubmit (direction ? direction : 0U, dataBytes, timeoutUs, &ocs);
  if (status != kIOReturnSuccess) return status;
  status = hostResponseHeader (0x21, lun, 0);
  if (status != kIOReturnSuccess) return status;
  const uint8_t *rsp = ucd + kRspOffset;
  *scsiStatus = (uint32_t (rsp[6]) << 8) | rsp[7];
  gRootDiagnostic.scsi = *scsiStatus;
  gRootDiagnostic.residual = loadBe32 (rsp + 12);
  unsigned length = (unsigned (rsp[10]) << 8) | rsp[11];
  if (length) {
    unsigned senseLength = (unsigned (rsp[32]) << 8) | rsp[33];
    if (length < 2 || senseLength > length - 2) return hostProtocolError (4);
    const uint8_t *sense = rsp + 34;
    if (senseLength >= 14 && ((sense[0] & 0x7f) == 0x70 || (sense[0] & 0x7f) == 0x71))
      gRootDiagnostic.sense = (uint32_t (sense[2] & 15) << 16) | (uint32_t (sense[12]) << 8) | sense[13];
    else if (senseLength >= 4 && ((sense[0] & 0x7f) == 0x72 || (sense[0] & 0x7f) == 0x73))
      gRootDiagnostic.sense = (uint32_t (sense[1] & 15) << 16) | (uint32_t (sense[2]) << 8) | sense[3];
  }
  if (!*scsiStatus && ((rsp[1] & 0x60) || gRootDiagnostic.residual)) return hostProtocolError (6);
  IOReturn result = *scsiStatus == 0 ? kIOReturnSuccess : kIOReturnNotReady;
  gRootDiagnostic.result = static_cast<uint32_t> (result);
  return result;
}

// The original read-only entry point: data, when present, always flows device -> host.
static IOReturn hostScsi (uint8_t lun, const uint8_t *cdb, unsigned cdbLength, bool toHost, uint32_t dataBytes,
                          uint32_t timeoutUs, uint32_t *scsiStatus)
{
  if (dataBytes && !toHost) { *scsiStatus = 0xffff; return kIOReturnBadArgument; }
  return hostScsiDir (lun, cdb, cdbLength, dataBytes ? 2U : 0U, dataBytes, timeoutUs, scsiStatus);
}

// Query read request (opcode 0x03 READ ATTRIBUTE / 0x05 READ FLAG). Returns the value field.
static IOReturn hostQueryRead (uint8_t opcode, uint8_t idn, uint32_t *value)
{
  if (!value || (opcode != 3 && opcode != 5)) return kIOReturnBadArgument;
  if (!hostClearUcd ()) return kIOReturnNotReady;
  uint8_t *ucd = gHost.virt + kUcdOffset;
  ucd[0] = 0x16;                      // QUERY REQUEST
  ucd[3] = 1;
  ucd[5] = 0x01;                      // standard read request (header dword 1, byte 1)
  ucd[12] = opcode;
  ucd[13] = idn;
  uint32_t ocs = 0xf;
  IOReturn status = hostSubmit (0, 0, 200000, &ocs);
  if (status != kIOReturnSuccess) {
    return status;
  }
  status = hostResponseHeader (0x36, 0, 1);
  if (status != kIOReturnSuccess) return status;
  const uint8_t *rsp = ucd + kRspOffset;
  if (rsp[10] || rsp[11]) return hostProtocolError (4);
  if (rsp[12] != opcode || rsp[13] != idn || rsp[14] || rsp[15]) return hostProtocolError (5);
  if (rsp[6] != 0) {
    gRootDiagnostic.result = static_cast<uint32_t> (kIOReturnError);
    return kIOReturnError;
  }
  *value = loadBe32 (rsp + 20);
  return kIOReturnSuccess;
}

// Read `blocks` (<= kDataMax / 4096) 4096-byte blocks of LUN `lun` starting at `lba` into dst.
static IOReturn hostRead4k (uint8_t lun, uint64_t lba, uint32_t blocks, void *dst)
{
  if (!gHost.ready || !dst || !blocks || blocks > kDataMax / 4096 || lba > 0xffffffffULL - (blocks - 1)) {
    return kIOReturnBadArgument;
  }
  IOLockLock (gHost.lock);
  gRootDiagnostic.lba = lba;
  uint8_t cdb[10] = { 0x28, 0, uint8_t (lba >> 24), uint8_t (lba >> 16), uint8_t (lba >> 8), uint8_t (lba),
                      0, uint8_t (blocks >> 8), uint8_t (blocks), 0 };
  uint32_t scsi = 0;
  IOReturn status = hostScsi (lun, cdb, 10, true, blocks * 4096U, 1000000, &scsi);
  if (status == kIOReturnSuccess) {
    memcpy (dst, gHost.virt + kDataOffset, blocks * 4096U);
  }
  IOLockUnlock (gHost.lock);
  return status;
}

// ---- Persistent store and log windows ------------------------------------------------------------------------------
// All media writes pass through this guard. Legacy LUN 0 store/log windows retain their policy.
// The optional LUN 4 path owns only preallocated macoslog file data, excluding its immutable guard.
// Each caller proves ownership before opening a window; every write is bounded again here.
static bool gStoreOpen = false, gLogOpen = false;
#if defined(NATIVE_MACOSLOG) && NATIVE_MACOSLOG
static bool gMacosLogOpen = false;
static uint64_t gMacosLogFirst, gMacosLogBlocks;
// Dedicated FAT file data on LUN 4; the final immutable guard block is excluded.
static bool hostMacosLogOpen(uint64_t first, uint64_t blocks) {
  if (gMacosLogOpen || blocks != 511 || first <= 448512 || first > 464896 - 512) return false;
  gMacosLogFirst = first; gMacosLogBlocks = blocks; gMacosLogOpen = true; return true;
}
#endif
static uint64_t gStoreFirst = 0, gStoreBlocks = 0, gLogFirst = 0, gLogBlocks = 0;

static bool hostLogOpen (uint64_t first, uint64_t blocks)
{
  if (gLogOpen || !blocks || first > 0xffffffffULL || blocks > 0xffffffffULL - first) return false;
  if (gStoreOpen && first < gStoreFirst + gStoreBlocks && gStoreFirst < first + blocks) return false;
  gLogFirst = first;
  gLogBlocks = blocks;
  gLogOpen = true;
  return true;
}

static bool hostInWindow (uint64_t lba, uint32_t blocks)
{
  if (gStoreOpen && lba >= gStoreFirst && lba - gStoreFirst < gStoreBlocks && blocks <= gStoreBlocks - (lba - gStoreFirst)) return true;
  return gLogOpen && lba >= gLogFirst && lba - gLogFirst < gLogBlocks && blocks <= gLogBlocks - (lba - gLogFirst);
}

static bool hostStoreOpen (uint64_t first, uint64_t blocks)
{
  if (gStoreOpen || !blocks || first > 0xffffffffULL || blocks > 0xffffffffULL - first) return false;
  if (gLogOpen && first < gLogFirst + gLogBlocks && gLogFirst < first + blocks) return false;
  gStoreFirst = first;
  gStoreBlocks = blocks;
  gStoreOpen = true;
  return true;
}

// WRITE(10) of 4096-byte blocks; both LUN and range must match an explicitly opened window.
static IOReturn hostWrite4k (uint8_t lun, uint64_t lba, uint32_t blocks, const void *src, bool fua = false)
{
  bool allowed = lun == 0 && hostInWindow(lba, blocks);
#if defined(NATIVE_MACOSLOG) && NATIVE_MACOSLOG
  if (lun == 4 && gMacosLogOpen && lba >= gMacosLogFirst && lba - gMacosLogFirst < gMacosLogBlocks)
    allowed = blocks <= gMacosLogBlocks - (lba - gMacosLogFirst);
#endif
  if (!gHost.ready || !src || !blocks || blocks > kDataMax / 4096 || !allowed) {
    return kIOReturnBadArgument;
  }
  IOLockLock (gHost.lock);
  gRootDiagnostic.lba = lba;
  memcpy (gHost.virt + kDataOffset, src, blocks * 4096U);
  uint8_t cdb[10] = { 0x2a, uint8_t (fua ? 0x08 : 0), uint8_t (lba >> 24), uint8_t (lba >> 16), uint8_t (lba >> 8), uint8_t (lba),
                      0, uint8_t (blocks >> 8), uint8_t (blocks), 0 };
  uint32_t scsi = 0;
  IOReturn status = hostScsiDir (lun, cdb, 10, 1, blocks * 4096U, 2000000, &scsi);
  IOLockUnlock (gHost.lock);
  return status;
}

// SYNCHRONIZE CACHE(10) for the whole LUN: everything written so far is on the medium when this returns.
static IOReturn hostSyncCache (void)
{
  if (!gHost.ready || (!gStoreOpen && !gLogOpen)) return kIOReturnNotReady;
  IOLockLock (gHost.lock);
  uint8_t cdb[10] = { 0x35 };
  uint32_t scsi = 0;
  IOReturn status = hostScsiDir (0, cdb, 10, 0, 0, 10000000, &scsi);
  IOLockUnlock (gHost.lock);
  return status;
}


#if defined(NATIVE_MACOSLOG) && NATIVE_MACOSLOG
// Clear initial LUN readiness conditions before any GPT read; writes are never retried.
static IOReturn hostMacosLogReady(void) {
  if (!gHost.ready) return kIOReturnNotReady;
  IOReturn status = kIOReturnNotReady;
  for (unsigned attempt = 0; attempt < 4; attempt++) {
    IOLockLock(gHost.lock);
    uint8_t cdb[6] = {}; uint32_t scsi = 0;
    status = hostScsi(4, cdb, 6, false, 0, 200000, &scsi);
    uint32_t sense = (gRootDiagnostic.sense >> 16) & 15;
    IOLockUnlock(gHost.lock);
    if (status == kIOReturnSuccess) return status;
    if (gHost.poisoned || status != kIOReturnNotReady || (sense != 2 && sense != 6)) break;
    IODelay(1000);
  }
  return status;
}
static IOReturn hostMacosLogSync(void) {
  if (!gHost.ready || !gMacosLogOpen) return kIOReturnNotReady;
  IOLockLock(gHost.lock);
  uint8_t cdb[10] = {0x35}; uint32_t scsi = 0;
  IOReturn status = hostScsiDir(4, cdb, 10, 0, 0, 10000000, &scsi);
  IOLockUnlock(gHost.lock);
  return status;
}
#endif
