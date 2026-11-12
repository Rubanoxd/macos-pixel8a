#!/usr/bin/env python3
"""Phase 4 (graphics driver): firmware events, job interrupt, blocking waits, tiler heap growth, display fence for WindowServer.
Applies to platform/akita/drivers/Mali/Mali.cpp + mali_uapi.h (both copies). Originals in scratch/phase4-orig. Local only (Generated-by: LLM)."""
import shutil
from pathlib import Path

R = Path('/Volumes/datos2/unixnu-akita')
MALI = R / 'ref/macOS-ForSheng/platform/akita/drivers/Mali/Mali.cpp'
UAPI = [R / 'ref/macOS-ForSheng/platform/akita/drivers/Mali/mali_uapi.h', R / 'ref/mali/Mali/mali_uapi.h']
ORIG = R / 'scratch/phase4-orig'

s = (ORIG / 'Mali.cpp').read_text()


def sub(old, new, count=1):
    global s
    assert s.count(old) == count, (s.count(old), old[:90])
    s = s.replace(old, new)


# ---- the bring-up allocation trace slept 4 x 150 ms in every memAlloc (page tables, rings...) under the driver lock: off by default
sub("""unsigned gTraceAlloc = 1;   // akita bring-up: trace allocations (with a pause so the USB log gets out before a hang)""",
    """unsigned gTraceAlloc = 0;   // akita bring-up: trace allocations (with a pause so the USB log gets out before a hang); boot-arg mali-trace-alloc=1
                            // (it was on by default: every memAlloc slept 600 ms under the driver lock, VM_BIND calls took up to 2.4 s)""")
sub("""  started = true;
  gLevel = on;""", """  started = true;
  gLevel = on;
  PE_parse_boot_argn("mali-trace-alloc", &gTraceAlloc, sizeof gTraceAlloc);   // akita""")

# ---- includes
sub('#include <kern/thread_call.h>\n', '#include <kern/thread_call.h>\n#include <kern/sched_prim.h>\n#include <kern/thread.h>\n')

# ---- registers and interface structures
sub('kJobIntRaw = 0x1000, kJobIntClear = 0x1004, kJobIntMask = 0x1008,', 'kJobIntRaw = 0x1000, kJobIntClear = 0x1004, kJobIntMask = 0x1008, kJobIntStat = 0x100c,')
sub('''  uint64_t faultInfo, fatalInfo;
};''', '''  uint64_t faultInfo, fatalInfo;
  uint32_t reserved3[10], heapVtStart, heapVtEnd, reserved4, heapFragEnd;   // akita: what a tiler that ran out of heap reports (Panthor's cs output)
  uint64_t heapAddress;
};''')
sub('''  kCsgEvents = kCsgSyncUpdate | kCsgIdle | kCsgProgressTimer,
};''', '''  kCsgEvents = kCsgSyncUpdate | kCsgIdle | kCsgProgressTimer,
  // akita: events the firmware raises on a queue (CS_ACK toggles them, the host answers in CS_REQ)
  kCsTilerOom = 1u << 26, kCsFatal = 1u << 30, kCsFault = 1u << 31,
  kCsEvents = kCsTilerOom | kCsFatal | kCsFault,
};''')

# ---- tiler heaps: remember the limits, add chunks later
sub('''  uint32_t chunkSize, chunkCount, maxChunks;
  Bo **chunks;''', '''  uint32_t chunkSize, chunkCount, maxChunks;
  uint32_t chunkCap, targetInFlight;   // akita: room in the two arrays; render passes in flight above which the heap does not grow
  Bo **chunks;''')
sub('''  if (h.chunks) IODelete(h.chunks, Bo *, h.chunkCount);
  if (h.chunkVa) IODeleteData(h.chunkVa, uint64_t, h.chunkCount);''', '''  if (h.chunks) IODelete(h.chunks, Bo *, h.chunkCap);
  if (h.chunkVa) IODeleteData(h.chunkVa, uint64_t, h.chunkCap);''')
sub('int heapCreate(Ctx &c, uint32_t initialChunks, uint32_t chunkSize, uint32_t maxChunks, void *owner, uint64_t *ctxVa, uint64_t *firstChunkVa) {',
    'int heapCreate(Ctx &c, uint32_t initialChunks, uint32_t chunkSize, uint32_t maxChunks, uint32_t targetInFlight, void *owner, uint64_t *ctxVa, uint64_t *firstChunkVa) {')
sub('''  h.ctx = &c; h.owner = owner; h.chunkSize = chunkSize; h.maxChunks = maxChunks;
  h.chunks = IONewZero(Bo *, initialChunks);
  h.chunkVa = IONewZeroData(uint64_t, initialChunks);
  h.used = true;
  if (!h.chunks || !h.chunkVa) { h.chunkCount = initialChunks; heapFree(h); return -1; }''',
    '''  h.ctx = &c; h.owner = owner; h.chunkSize = chunkSize; h.maxChunks = maxChunks; h.targetInFlight = targetInFlight;
  h.chunks = IONewZero(Bo *, initialChunks);
  h.chunkVa = IONewZeroData(uint64_t, initialChunks);
  h.chunkCap = initialChunks;
  h.used = true;
  if (!h.chunks || !h.chunkVa) { heapFree(h); return -1; }''')
sub('''  return static_cast<int>(slot);
}

// What lives in the MCU's address space for a CSG slot''', '''  return static_cast<int>(slot);
}

// akita (Generated-by: LLM): one more chunk for a heap whose tiler ran out (Panthor's panthor_heap_grow). The firmware chains it itself, so
// its header's link stays zero. Returns what the firmware wants to hear, the chunk's address ORed with its size in pages, or 0.
uint64_t heapAddChunk(Heap &h) {
  if (h.chunkCount >= h.maxChunks) return 0;
  if (h.chunkCount == h.chunkCap) {
    const uint32_t cap = h.chunkCap * 2 > h.maxChunks ? h.maxChunks : h.chunkCap * 2;
    Bo **chunks = IONewZero(Bo *, cap);
    uint64_t *vas = IONewZeroData(uint64_t, cap);
    if (!chunks || !vas) {
      if (chunks) IODelete(chunks, Bo *, cap);
      if (vas) IODeleteData(vas, uint64_t, cap);
      return 0;
    }
    memcpy(chunks, h.chunks, h.chunkCount * sizeof(Bo *));
    memcpy(vas, h.chunkVa, h.chunkCount * sizeof(uint64_t));
    IODelete(h.chunks, Bo *, h.chunkCap);
    IODeleteData(h.chunkVa, uint64_t, h.chunkCap);
    h.chunks = chunks; h.chunkVa = vas; h.chunkCap = cap;
  }
  Bo *bo = boCreate(h.chunkSize, nullptr);
  if (!bo) return 0;
  memset(bo->kva, 0, 64);
  __asm__ volatile("dsb sy" ::: "memory");
  const uint64_t va = ctxKernelVaAligned(*h.ctx, h.chunkSize, h.chunkSize >= 0x200000 ? 0x200000 : 0x1000);
  if (!ctxMapKernelBo(*h.ctx, bo, va, kLeafCached)) { boRelease(bo); return 0; }
  h.chunks[h.chunkCount] = bo; h.chunkVa[h.chunkCount] = va;
  ++h.chunkCount;
  return (va & ~0xfffull) | (h.chunkSize >> 12);
}

// What lives in the MCU's address space for a CSG slot''')
sub('heapCreate(*c, i.initial_chunk_count, i.chunk_size, i.max_chunks, this, &o.ctx_va, &o.first_chunk_va);',
    'heapCreate(*c, i.initial_chunk_count, i.chunk_size, i.max_chunks, i.target_in_flight, this, &o.ctx_va, &o.first_chunk_va);')

# ---- the events block, after gpuFaultScan
EVENTS = r'''
// ---- akita (Generated-by: LLM, local only): the firmware's events, the job interrupt, and waits that sleep ------------------------------
// Until here nothing listened to the firmware: the job interrupt was masked, a group's events were answered only when its next job was
// submitted, a queue's (fault, fatal, tiler out of memory) never, and every wait was a poll. Now:
//  - gpuEventsProcess() (driver lock held) answers what the groups in the slots raised and gives a tiler that ran out one more chunk;
//  - the job interrupt (zuma: SPI 347, level high) clears its source, moves gEventGen on, wakes the waits and queues gpuEventsProcess();
//  - a wait (a fence, MALI_EVENT_WAIT, the display's fence) sleeps on gEventGen, with a short time limit as its safety net.
// Boot-args: mali-events=0     none of this (the driver as it was: the interrupt masked, polls of 1 ms)
//            mali-irq=0        the events are processed on the clients' calls and inside the waits, without the interrupt
//            mali-heap-grow=0  a tiler heap keeps its initial chunks
extern "C" IOReturn armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *), void *context, uint32_t *readback, uint64_t *router);
extern "C" void armcpu_spi_disable(uint32_t intid);

constexpr uint32_t kJobIntid = 32 + 347;   // IRQ_G3D_IRQJOB_G3D
bool gEventsOn;
unsigned gHeapGrow = 1;
volatile uint64_t gEventGen;       // moves on whenever something a wait may be waiting for can have happened
int gEventChannel;                 // what the waits sleep on
volatile uint32_t gIrqLive;        // 1: the job interrupt is attached and unmasked
volatile uint32_t gIrqGaveUp;      // why it was masked again: 1 fired with nothing pending, 2 a storm, 3 never fired
uint64_t gIrqTaken, gIrqEmpty, gIrqWindow, gIrqInWindow;   // the handler's own (boot CPU, interrupts off)
uint32_t gIrqEmptyRun;
thread_call_t gEventCall;
uint64_t gHeapGrown, gHeapRefused, gEventsAnswered, gWaitSlept, gWaitTimedOut;
volatile uint32_t gIrqMissed;      // waits whose condition came true during a sleep that no interrupt ended, while none has ever fired

inline uint64_t ticksNow() {
  uint64_t v;
  __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v));
  return v;
}
inline uint64_t ticksPerSecond() {
  static uint64_t f;
  if (!f) __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
  return f;
}

inline void eventsMoved() {
  __atomic_fetch_add(&gEventGen, 1ull, __ATOMIC_SEQ_CST);
  thread_wakeup(static_cast<event_t>(&gEventChannel));
}

void irqGiveUp(uint32_t why) {
  gpuWrite(kJobIntMask, 0);
  armcpu_spi_disable(kJobIntid);
  __atomic_store_n(&gIrqGaveUp, why, __ATOMIC_RELEASE);
  __atomic_store_n(&gIrqLive, 0u, __ATOMIC_RELEASE);
}

// The primary interrupt context of the boot CPU, before the EOI: no locks, no memory, no printing. The line is level triggered: writing
// the status back to JOB_INT_CLEAR drops it. A line that fires with nothing of ours pending 64 times in a row, or more than 100000 times
// in a second, is taken out of service (a wrong INTID or a source that cannot be cleared must not hold the boot CPU): the waits then poll.
void jobInterrupt(void *) {
  const uint32_t status = gpu(kJobIntStat);
  if (!status) {
    ++gIrqEmpty;
    if (++gIrqEmptyRun >= 64) { irqGiveUp(1); eventsMoved(); }
    return;
  }
  gIrqEmptyRun = 0;
  gpuWrite(kJobIntClear, status);
  ++gIrqTaken;
  const uint64_t now = ticksNow();
  if (now - gIrqWindow > ticksPerSecond()) { gIrqWindow = now; gIrqInWindow = 0; }
  if (++gIrqInWindow > 100000) irqGiveUp(2);
  eventsMoved();
  if (gEventCall) thread_call_enter(gEventCall);
}

// Panthor's group_process_tiler_oom(): the tiler of queue q has no chunk left. Give it one more unless the heap is at its limit or has more
// render passes in flight than its target; with 0 the firmware waits for fragment jobs to hand chunks back or runs the client's own handler
// (incremental rendering). Driver lock held.
void heapOutOfMemory(Group &g, unsigned q, const CsView &c) {
  const uint64_t heapVa = c.out->heapAddress;
  const uint32_t vtStart = c.out->heapVtStart, vtEnd = c.out->heapVtEnd, fragEnd = c.out->heapFragEnd;
  Heap *h = nullptr;
  for (unsigned i = 0; i < kMaxHeaps && !h; ++i)
    if (gHeap[i].used && gHeap[i].ctx == g.ctx && gHeap[i].ctxVa == heapVa) h = &gHeap[i];
  uint64_t chunk = 0;
  const char *why = "one more chunk";
  if (!h) why = "no such heap";
  else if (fragEnd > vtEnd || vtEnd >= vtStart) why = "counters that make no sense";
  else if (!gHeapGrow) why = "left as it is (mali-heap-grow=0)";
  else if (vtStart - fragEnd > h->targetInFlight) why = "more render passes in flight than its target";
  else if (h->chunkCount >= h->maxChunks) why = "at its limit";
  else if (!(chunk = heapAddChunk(*h))) why = "no memory for a chunk";
  if (chunk) ++gHeapGrown; else ++gHeapRefused;
  c.in->heapStart = chunk;
  c.in->heapEnd = chunk;
  __asm__ volatile("dsb sy" ::: "memory");
  static unsigned shown;
  if (shown < 24) {
    ++shown;
    printf("mali: tiler of group %u queue %u out of heap (%u render passes in flight, %u fragment jobs to go, heap %#llx with %u of %u chunks): %s\n", g.id, q, vtStart - fragEnd,
           vtEnd - fragEnd, static_cast<unsigned long long>(heapVa), h ? h->chunkCount : 0u, h ? h->maxChunks : 0u, why);
  }
}

// Answer what the firmware raised on the groups that sit in slots (Panthor's sched_process_csg_irq_locked). Driver lock held.
void gpuEventsProcess(const char *when) {
  if (!gEventsOn || !gGlbControl) return;
  bool faults = false;
  for (unsigned slot = 0; slot < kMaxGroups; ++slot) {
    Group *g = gSlotGroup[slot];
    SlotView s;
    if (!g || !slotView(slot, s)) continue;
    // The queues' interrupt requests are answered before the queues are looked at: one raised while we look is then seen the next time.
    const uint32_t csIrqs = s.out->csIrqReq;
    if (csIrqs != s.in->csIrqAck) s.in->csIrqAck = csIrqs;
    uint32_t ring = 0;
    for (unsigned q = 0; q < g->queues; ++q) {
      CsView c;
      if (!csView(s, q, c)) continue;
      const uint32_t ack = c.out->ack, events = (c.in->req ^ ack) & kCsEvents;
      if (!events) continue;
      if (events & kCsTilerOom) heapOutOfMemory(*g, q, c);
      if (events & (kCsFault | kCsFatal)) faults = true;
      c.in->req = (c.in->req & ~events) | (ack & events);
      if (events & (kCsTilerOom | kCsFault)) ring |= 1u << q;   // the queue goes on once it is told so
      ++gEventsAnswered;
    }
    const uint32_t events = (s.out->ack ^ s.in->req) & kCsgEvents;
    if (events) { s.in->req ^= events; ++gEventsAnswered; }
    if (events & kCsgProgressTimer) {
      static unsigned shown;
      if (shown++ < 8) printf("mali: group %u (slot %u) made no progress within the firmware's timer [%s]\n", g->id, slot, when);
    }
    if (ring) {
      s.in->doorbellReq = (s.in->doorbellReq & ~ring) | ((~s.out->doorbellAck) & ring);
      __asm__ volatile("dsb sy" ::: "memory");
      ringGlobalDoorbell(slot);
    }
  }
  __asm__ volatile("dsb sy" ::: "memory");
  if (faults) gpuFaultScan(when);
}

extern IOLock *gLock;

void gpuEventWork(thread_call_param_t, thread_call_param_t) {
  if (!gLock) return;
  IOLockLock(gLock);
  gpuEventsProcess("interrupt");
  IOLockUnlock(gLock);
  eventsMoved();   // after the answers: what happened between the firmware's event and its answer raised no interrupt of its own
}

// Without the interrupt: the events are looked at from the waits, if no call is in the driver right now.
void eventsPoll() {
  if (!gEventsOn || !gLock || !IOLockTryLock(gLock)) return;
  gpuEventsProcess("poll");
  IOLockUnlock(gLock);
}

// Sleep until the events move on from `gen` or `us` microseconds pass. No lock held.
void eventSleep(uint64_t gen, uint64_t us) {
  uint64_t deadline;
  clock_interval_to_deadline(static_cast<uint32_t>(us ? us : 1), NSEC_PER_USEC, &deadline);
  assert_wait_deadline(static_cast<event_t>(&gEventChannel), THREAD_UNINT, deadline);
  if (__atomic_load_n(&gEventGen, __ATOMIC_ACQUIRE) != gen) {
    clear_wait(current_thread(), THREAD_AWAKENED);
    return;
  }
  ++gWaitSlept;
  if (thread_block(THREAD_CONTINUE_NULL) == THREAD_TIMED_OUT) ++gWaitTimedOut;
}

// Wait until done() or the time is up; true when done. `spins` looks of 10 microseconds come first (a short job is done within tens of
// microseconds); then the thread sleeps: until the next event, at most 4 ms, with the interrupt; 1 ms at a time without it, as before.
template <typename Condition> bool eventWaitFor(uint64_t timeoutUs, unsigned spins, Condition done) {
  const uint64_t freq = ticksPerSecond(), begin = ticksNow();
  for (unsigned turn = 0;; ++turn) {
    const uint64_t gen = __atomic_load_n(&gEventGen, __ATOMIC_ACQUIRE);
    if (done()) return true;
    const uint64_t waited = (ticksNow() - begin) * 1000000 / freq;
    if (waited >= timeoutUs) return false;
    if (turn < spins) { IODelay(10); continue; }
    if (!gEventsOn) { IOSleep(1); continue; }
    const bool live = __atomic_load_n(&gIrqLive, __ATOMIC_ACQUIRE) != 0;
    if (!live) eventsPoll();
    const uint64_t left = timeoutUs - waited, slice = live ? 4000 : 1000;
    const uint64_t taken = gIrqTaken;
    eventSleep(gen, left < slice ? left : slice);
    // An interrupt that never comes (the line is the secure world's, or not the GPU's) must not leave every wait at 4 ms: after a few
    // waits that only the time limit ended while their condition had come true, go back to polling.
    if (live && !taken && !gIrqTaken && __atomic_load_n(&gEventGen, __ATOMIC_ACQUIRE) == gen && done()) {
      if (__atomic_add_fetch(&gIrqMissed, 1u, __ATOMIC_RELAXED) == 16) {
        const boolean_t was = ml_set_interrupts_enabled(FALSE);
        irqGiveUp(3);
        (void)ml_set_interrupts_enabled(was);
      }
      return true;
    }
  }
}

// Attach the job interrupt and prove it with a ping: the firmware answers one through the global interface's interrupt (bit 31).
// A line that stays silent is taken out of service at once. Thread context; the driver lock is taken for the ping.
unsigned gIrqArg = 1;
void irqAttach() {
  if (!gEventsOn || !gLock || !gGlbInput || !gGlbOutput || gIrqLive || gIrqGaveUp || gEventCall) return;
  gEventCall = thread_call_allocate(gpuEventWork, nullptr);
  if (!gEventCall) { printf("mali: events: no thread call, polled\n"); return; }
  gpuWrite(kJobIntMask, 0);
  uint32_t readback = 0;
  uint64_t router = 0;
  __atomic_store_n(&gIrqLive, 1u, __ATOMIC_RELEASE);
  const IOReturn r = armcpu_spi_attach(kJobIntid, false, jobInterrupt, nullptr, &readback, &router);
  if (r != kIOReturnSuccess) {
    __atomic_store_n(&gIrqLive, 0u, __ATOMIC_RELEASE);
    printf("mali: events: job interrupt %u not attached (%#x), polled\n", kJobIntid, r);
    return;
  }
  IOLockLock(gLock);
  const uint64_t before = gIrqTaken;
  gpuWrite(kJobIntMask, 0xffffffffu);   // what is pending already fires now: its events are answered by the work it queues
  gGlbInput->req = (gGlbInput->req & ~kGlbPing) | ((gGlbOutput->ack ^ kGlbPing) & kGlbPing);
  __asm__ volatile("dsb sy" ::: "memory");
  gpuWrite(kDoorbell0, 1);
  bool fired = false;
  for (unsigned ms = 0; ms < 100 && !fired; ++ms) { IOSleep(1); fired = gIrqTaken != before; }
  if (!fired) {
    const boolean_t was = ml_set_interrupts_enabled(FALSE);
    irqGiveUp(3);
    (void)ml_set_interrupts_enabled(was);
  }
  gpuEventsProcess("the interrupt's start");
  IOLockUnlock(gLock);
  eventsMoved();
  printf("mali: events: job interrupt %u attached (GIC group/enable/trigger/priority %#x, routed to %#llx): the ping's interrupt %s (job irq raw %#x), tiler heaps %s\n", kJobIntid,
         readback, static_cast<unsigned long long>(router), fired ? "ARRIVED" : "did NOT arrive: taken out of service, polled", gpu(kJobIntRaw), gHeapGrow ? "grow" : "do not grow");
}

// Called once, when the service is about to be published. mali-irq=2 leaves the interrupt for later: sysctl kern.akita_mali=100
// attaches it on a running system (a first try with the log already on the wire).
void irqStart() {
  unsigned events = 1;
  PE_parse_boot_argn("mali-events", &events, sizeof events);
  PE_parse_boot_argn("mali-irq", &gIrqArg, sizeof gIrqArg);
  PE_parse_boot_argn("mali-heap-grow", &gHeapGrow, sizeof gHeapGrow);
  gEventsOn = events != 0;
  if (!gEventsOn) { printf("mali: events: off (mali-events=0): the firmware's events are not answered, the waits poll\n"); return; }
  if (gIrqArg != 1) {
    printf("mali: events: polled (mali-irq=%u%s), tiler heaps %s\n", gIrqArg, gIrqArg == 2 ? ": sysctl kern.akita_mali=100 attaches the interrupt" : "", gHeapGrow ? "grow" : "do not grow");
    return;
  }
  irqAttach();
}

// A line with the totals when they moved, from the calls' ten second table.
void eventsReport() {
  static uint64_t lastTaken, lastAnswered, lastGrown, lastRefused;
  static uint32_t toldGaveUp;
  const uint32_t gaveUp = __atomic_load_n(&gIrqGaveUp, __ATOMIC_ACQUIRE);
  if (gaveUp && !toldGaveUp) {
    toldGaveUp = 1;
    printf("mali: events: the job interrupt was taken out of service (%s; %llu taken, %llu with nothing pending): polled from here on\n",
           gaveUp == 1 ? "it fired with nothing pending" : gaveUp == 2 ? "a storm" : "it never fired", static_cast<unsigned long long>(gIrqTaken),
           static_cast<unsigned long long>(gIrqEmpty));
  }
  if (gIrqTaken == lastTaken && gEventsAnswered == lastAnswered && gHeapGrown == lastGrown && gHeapRefused == lastRefused) return;
  lastTaken = gIrqTaken; lastAnswered = gEventsAnswered; lastGrown = gHeapGrown; lastRefused = gHeapRefused;
  printf("mali: events: %llu interrupts (%llu empty), %llu events answered, waits slept %llu times (%llu ended by the time limit), tiler chunks added %llu refused %llu\n",
         static_cast<unsigned long long>(gIrqTaken), static_cast<unsigned long long>(gIrqEmpty), static_cast<unsigned long long>(gEventsAnswered),
         static_cast<unsigned long long>(gWaitSlept), static_cast<unsigned long long>(gWaitTimedOut), static_cast<unsigned long long>(gHeapGrown),
         static_cast<unsigned long long>(gHeapRefused));
}

// ---- M3a self-test
'''
sub('\n// ---- M3a self-test\n', EVENTS)

# gLock: declared extern above (inside the anonymous namespace), defined below as before
sub('IOLock *gLock;   // the driver\'s state is global', 'IOLock *gLock;   // the driver\'s state is global')

# ---- start the interrupt when the service comes up
sub('''      if (gLock && serviceCreate()) {
        if (gLevel == 7) m3b();''', '''      if (gLock && serviceCreate()) {
        irqStart();   // akita
        if (gLevel == 7) m3b();''')

# ---- sysctl kern.akita_mali=100: attach the interrupt now (mali-irq=2)
sub("""  static bool started;
  if (!on || started) {""", """  static bool started;
  if (on == 100 && started) {   // akita: see irqStart()
    if (gIrqArg == 2) irqAttach(); else printf("mali: level 100 ignored (mali-irq=%u)\\n", gIrqArg);
    return;
  }
  if (!on || started) {""")

# ---- the fence wait sleeps
sub('''  uint64_t waited = 0;
  while (*done < i.seqno && waited < i.timeout_us) {
    if (waited < 200) { IODelay(10); waited += 10; }
    else { IOSleep(1); waited += 1000; }
  }
  o->done_seqno = *done;''', '''  const uint64_t wanted = i.seqno;
  (void)eventWaitFor(i.timeout_us, 20, [done, wanted] { return *done >= wanted; });   // akita: sleeps until the job interrupt
  o->done_seqno = *done;''')

# ---- MALI_EVENT_WAIT, outside the driver's lock like the fence wait
sub('''  } else if (selector == MALI_FENCE_WAIT) {
    mali_fence_wait_in in;''', '''  } else if (selector == MALI_EVENT_WAIT) {
    // akita: what the user side's fence waits sleep in (darwin-drm keeps the fences itself: it only needs to know that something finished).
    mali_event_wait_in in;
    mali_event_wait_out out = {};
    if (a->structureInputSize < sizeof in || a->structureOutputSize < sizeof out || !a->structureInput || !a->structureOutput) return kIOReturnBadArgument;
    memcpy(&in, a->structureInput, sizeof in);
    const uint64_t from = in.gen;
    if (gEventsOn) (void)eventWaitFor(in.timeout_us > 1000000 ? 1000000 : in.timeout_us, 0, [from] { return __atomic_load_n(&gEventGen, __ATOMIC_ACQUIRE) != from; });
    out.gen = __atomic_load_n(&gEventGen, __ATOMIC_ACQUIRE);
    out.interrupts = gEventsOn ? __atomic_load_n(&gIrqLive, __ATOMIC_ACQUIRE) : 0;
    memcpy(a->structureOutput, &out, sizeof out);
    outSize = sizeof out;
    r = kIOReturnSuccess;
  } else if (selector == MALI_FENCE_WAIT) {
    mali_fence_wait_in in;''')
sub('''      gpuStallScan();
      if (r != kIOReturnNotReady || tries >= 10000) break;''', '''      gpuStallScan();
      if (!gIrqLive) gpuEventsProcess("a call");   // akita: without the interrupt the events are answered here
      if (r != kIOReturnNotReady || tries >= 10000) break;''')
sub('if (selector != MALI_LOG && selector != MALI_GOP) callStatRecord', 'if (selector != MALI_LOG && selector != MALI_GOP && selector != MALI_EVENT_WAIT) callStatRecord')
sub('''      if (line[0]) printf("mali: calls (selector:count total/lock-wait/max):%s\\n", line);''',
    '''      if (line[0]) printf("mali: calls (selector:count total/lock-wait/max):%s\\n", line);
      if (half == 0) eventsReport();''')

# ---- the display's fence: the render server is WindowServer on macOS
sub('''extern "C" int mali_fence_take(uint64_t *entries, unsigned cap) {''', '''// akita: the process whose frames reach the panel is backboardd on iOS and WindowServer on macOS.
static bool renderServerName(const char *name) { return strcmp(name, "backboardd") == 0 || strcmp(name, "WindowServer") == 0; }

extern "C" int mali_fence_take(uint64_t *entries, unsigned cap) {''')
sub('if (!gr.owner || strcmp(static_cast<MaliClient *>(gr.owner)->name(), "backboardd") != 0) continue;',
    'if (!gr.owner || !renderServerName(static_cast<MaliClient *>(gr.owner)->name())) continue;', 2)
sub('''// What a fence that gave up was waiting for (the first few times only)''', '''// akita (Generated-by: LLM): the display's wait for a fence taken with mali_fence_take: 1 as soon as it is done (the job interrupt ends the
// sleep), 0 when `us` microseconds passed first. Sleeps: not for an interrupt context, and no lock of the caller's may be one the driver needs.
extern "C" int mali_fence_wait(const uint64_t *entries, unsigned count, uint64_t us) {
  const uint64_t freq = ticksPerSecond(), begin = ticksNow();
  for (;;) {
    const uint64_t gen = __atomic_load_n(&gEventGen, __ATOMIC_ACQUIRE);
    const int state = mali_fence_done(entries, count);
    if (state == 1) return 1;
    const uint64_t waited = (ticksNow() - begin) * 1000000 / freq;
    if (waited >= us) return 0;
    const uint64_t left = us - waited;
    // -1: a call holds the driver's lock: look again soon. Else until the next event (4 ms at most), or 1 ms without the interrupt.
    const uint64_t slice = state < 0 ? 200 : (gEventsOn && __atomic_load_n(&gIrqLive, __ATOMIC_ACQUIRE)) ? 4000 : 1000;
    if (!gEventsOn) { IOSleep(1); continue; }
    eventSleep(gen, left < slice ? left : slice);
  }
}

// What the display needs to know to pace itself: the count of job interrupts taken so far (0 forever: there is none).
extern "C" uint64_t mali_event_interrupts(void) { return gIrqTaken; }

// 1 once the driver serves clients (before that mali_fence_take's -1 would mean "busy" forever).
extern "C" int mali_fence_available(void) { return gLock != nullptr; }

// What a fence that gave up was waiting for (the first few times only)''')

# ---- akita fullos157: a job can wait, on the GPU, for other queues of its group (SYNC_WAIT64 on their syncobj words) instead of the
# user side blocking the submitting thread until the GPU has finished them (darwin-drm's process_waits did that for every semaphore
# wait: each command buffer's submit waited for the previous one to finish, the GPU idled between them). Jobs grow to 256 bytes.
sub("""constexpr unsigned kJobSlots = 16, kJobBytes = kJobSlots * 8;   // Panthor pads every job to two cache lines""",
    """constexpr unsigned kJobSlots = 32, kJobBytes = kJobSlots * 8;   // Panthor pads every job to two cache lines; akita: four, for the waits""")
sub("""bool groupSubmit(Group &g, unsigned q, uint64_t csVa, uint32_t csSize, uint32_t latestFlush, uint64_t *seqno) {
  if (q >= g.queues) return false;""",
    """bool groupSubmit(Group &g, unsigned q, uint64_t csVa, uint32_t csSize, uint32_t latestFlush, uint64_t *seqno,
                 const mali_group_submit_wait *waits = nullptr, unsigned waitCount = 0) {
  if (q >= g.queues || waitCount > MALI_SUBMIT_MAX_WAITS) return false;""")
sub("""  uint64_t instrs[kJobSlots] = {};
  unsigned n = 0;
  instrs[n++] = (2ull << 56) | (valReg << 48) | latestFlush;                       // MOV32 val, latest_flush""",
    """  uint64_t instrs[kJobSlots] = {};
  unsigned n = 0;
  for (unsigned w = 0; w < waitCount; ++w) {   // akita: first the other queues this job depends on (before the flush below)
    if (waits[w].queue >= g.queues || waits[w].queue == q || !waits[w].seqno) continue;
    instrs[n++] = (1ull << 56) | (addrReg << 48) | (g.syncVa + waits[w].queue * 16);   // MOV48 addr, that queue's sync word
    instrs[n++] = (1ull << 56) | (valReg << 48) | ((waits[w].seqno - 1) & 0xffffffffffffull);   // MOV48 val, seqno - 1
    instrs[n++] = (53ull << 56) | (addrReg << 40) | (valReg << 32) | (1ull << 28);       // SYNC_WAIT64.gt addr, val
  }
  instrs[n++] = (2ull << 56) | (valReg << 48) | latestFlush;                       // MOV32 val, latest_flush""")
sub("""      if (!groupSubmit(*g, i.queue, i.cs_va, i.cs_size, i.latest_flush, &o.seqno)) return kIOReturnNoSpace;""",
    """      mali_group_submit2_in i2 = {};   // akita: the longer form carries the waits on other queues
      if (inSize >= sizeof i2) {
        memcpy(&i2, in, sizeof i2);
        if (i2.wait_count > MALI_SUBMIT_MAX_WAITS) return kIOReturnBadArgument;
        for (unsigned w = 0; w < i2.wait_count; ++w) if (i2.waits[w].queue >= g->queues) return kIOReturnBadArgument;
      }
      if (!groupSubmit(*g, i.queue, i.cs_va, i.cs_size, i.latest_flush, &o.seqno, i2.waits, i2.wait_count)) return kIOReturnNoSpace;""")

# ---- akita fullos162: the GPU's LATEST_FLUSH_ID page (GPU base + 0x10000, as Panthor maps it at DRM_PANTHOR_USER_FLUSH_ID_MMIO_OFFSET),
# read-only and uncached, for the user side's latest_flush: a job's opening FLUSH_CACHE2 clean_inv_all is then skipped by the GPU when the
# caches were flushed since the submit, instead of every job (~170 a frame) cleaning and invalidating the whole L2 (latest_flush 0).
sub("""    } else if (type & MALI_MAP_SYNCOBJ) {
      Group *g = groupFor(type & 0xff);""", """    } else if (type == MALI_MAP_FLUSH_ID) {   // akita
      static IOMemoryDescriptor *flushIdPage;
      if (!flushIdPage) flushIdPage = IOMemoryDescriptor::withPhysicalAddress(kGpuPhys + 0x10000, 0x4000, kIODirectionIn);
      md = flushIdPage;
      mapOptions = kIOMapInhibitCache | kIOMapReadOnly;
    } else if (type & MALI_MAP_SYNCOBJ) {
      Group *g = groupFor(type & 0xff);""")

# ---- akita fullos160: where a submit's time goes inside the driver (cntvct ticks): activate, ring write, kick, fault scan, stall scan,
# events, the whole dispatch; reported with the call statistics every 10 s.
sub("""bool groupSubmit(Group &g, unsigned q, uint64_t csVa, uint32_t csSize, uint32_t latestFlush, uint64_t *seqno,""",
    """uint64_t gSubPh[8];   // akita: 0 activate, 1 ring write, 2 kick, 3 fault scan, 4 stall scan, 5 events, 6 dispatch, 7 count
static inline uint64_t cntvctNow() { return mach_absolute_time(); }   // (cntvct_el0 read here went backwards: the kernel's own clock)
uint64_t gSubNeg;   // akita: spans whose end read an earlier counter than their start (cores whose counters disagree?)
static inline void subPh(unsigned i, uint64_t from, uint64_t to) { if (to >= from) gSubPh[i] += to - from; else gSubNeg++; }
bool groupSubmit(Group &g, unsigned q, uint64_t csVa, uint32_t csSize, uint32_t latestFlush, uint64_t *seqno,""")
sub("""  ri->extract = ro->extract;
  ri->insert = qu.insert;
  __asm__ volatile("dsb sy" ::: "memory");
  kickStream(s, g.csg, q);
  return true;""", """  ri->extract = ro->extract;
  ri->insert = qu.insert;
  __asm__ volatile("dsb sy" ::: "memory");
  const uint64_t kick0 = cntvctNow();
  kickStream(s, g.csg, q);
  subPh(2, kick0, cntvctNow());
  return true;""")
sub("""      mali_group_submit_out o = {0};
      const int act = groupActivate(*g);""", """      mali_group_submit_out o = {0};
      const uint64_t act0 = cntvctNow();
      const int act = groupActivate(*g);
      subPh(0, act0, cntvctNow());""")
sub("""      if (!groupSubmit(*g, i.queue, i.cs_va, i.cs_size, i.latest_flush, &o.seqno, i2.waits, i2.wait_count)) return kIOReturnNoSpace;""",
    """      const uint64_t sub0 = cntvctNow(), kick0 = gSubPh[2];
      if (!groupSubmit(*g, i.queue, i.cs_va, i.cs_size, i.latest_flush, &o.seqno, i2.waits, i2.wait_count)) return kIOReturnNoSpace;
      subPh(1, sub0 + (gSubPh[2] - kick0), cntvctNow());""")
sub("""      r = closed ? static_cast<IOReturn>(kIOReturnNotAttached)
                 : dispatch(selector, static_cast<const uint8_t *>(a->structureInput), a->structureInputSize, static_cast<uint8_t *>(a->structureOutput), a->structureOutputSize, &outSize);
      if (selector == MALI_GROUP_SUBMIT || selector == MALI_GROUP_DESTROY) gpuFaultScan(selector == MALI_GROUP_SUBMIT ? "after a submit" : "after a group destroy");
      gpuStallScan();
      if (!gIrqLive) gpuEventsProcess("a call");   // akita: without the interrupt the events are answered here""",
    """      const uint64_t d0 = cntvctNow();
      r = closed ? static_cast<IOReturn>(kIOReturnNotAttached)
                 : dispatch(selector, static_cast<const uint8_t *>(a->structureInput), a->structureInputSize, static_cast<uint8_t *>(a->structureOutput), a->structureOutputSize, &outSize);
      const uint64_t d1 = cntvctNow();
      if (selector == MALI_GROUP_SUBMIT || selector == MALI_GROUP_DESTROY) gpuFaultScan(selector == MALI_GROUP_SUBMIT ? "after a submit" : "after a group destroy");
      const uint64_t d2 = cntvctNow();
      gpuStallScan();
      const uint64_t d3 = cntvctNow();
      if (!gIrqLive) gpuEventsProcess("a call");   // akita: without the interrupt the events are answered here
      if (selector == MALI_GROUP_SUBMIT) { subPh(6, d0, d1); subPh(3, d1, d2); subPh(4, d2, d3); subPh(5, d3, cntvctNow()); gSubPh[7]++; }""")
sub("""      if (line[0]) printf("mali: calls (selector:count total/lock-wait/max):%s\\n", line);""",
    """      if (line[0]) printf("mali: calls (selector:count total/lock-wait/max):%s\\n", line);
      if (half == 1 && gSubPh[7]) {   // akita: a submit's parts, mean ns
        const uint64_t n = gSubPh[7]; uint64_t f = 0; absolutetime_to_nanoseconds(1000000000ull, &f); f = f ? 1000000000ull * 1000000000ull / f : gCallStatFreq;   // ticks per second of mach_absolute_time
        printf("mali: submit parts (mean ns of %llu): dispatch %llu (activate %llu, ring %llu, kick %llu), fault scan %llu, stall scan %llu, events %llu, backwards %llu\\n",
               (unsigned long long)n, (unsigned long long)(gSubPh[6] * 1000000ull / f * 1000 / n), (unsigned long long)(gSubPh[0] * 1000000ull / f * 1000 / n),
               (unsigned long long)(gSubPh[1] * 1000000ull / f * 1000 / n), (unsigned long long)(gSubPh[2] * 1000000ull / f * 1000 / n),
               (unsigned long long)(gSubPh[3] * 1000000ull / f * 1000 / n), (unsigned long long)(gSubPh[4] * 1000000ull / f * 1000 / n),
               (unsigned long long)(gSubPh[5] * 1000000ull / f * 1000 / n), (unsigned long long)gSubNeg);
      }""")

# ---- akita fullos167: a batch of submits for one group in one call (MALI_GROUP_SUBMIT_BATCH): one IOKit round trip (~20 us each) and
# one doorbell per queue instead of one per stream; the MMU fault scan after a submit (one MMIO read, ~7.6 us) at most every 5 ms.
sub("""bool groupSubmit(Group &g, unsigned q, uint64_t csVa, uint32_t csSize, uint32_t latestFlush, uint64_t *seqno,
                 const mali_group_submit_wait *waits = nullptr, unsigned waitCount = 0) {""",
    """bool groupSubmit(Group &g, unsigned q, uint64_t csVa, uint32_t csSize, uint32_t latestFlush, uint64_t *seqno,
                 const mali_group_submit_wait *waits = nullptr, unsigned waitCount = 0, bool kick = true) {""")
sub("""  const uint64_t kick0 = cntvctNow();
  kickStream(s, g.csg, q);
  subPh(2, kick0, cntvctNow());
  return true;
}""", """  if (!kick) return true;   // akita: the batch rings once per queue (kickStreams)
  const uint64_t kick0 = cntvctNow();
  kickStream(s, g.csg, q);
  subPh(2, kick0, cntvctNow());
  return true;
}

// akita: kickStream for several streams of a group's slot at once (the queues in mask): one global and one group doorbell.
void kickStreams(Group &g, uint32_t mask) {
  SlotView s;
  if (!mask || g.csg == kNoSlot || !slotView(g.csg, s)) return;
  const uint32_t events = (s.out->ack ^ s.in->req) & kCsgEvents;
  if (events) s.in->req ^= events;
  s.in->doorbellReq = (s.in->doorbellReq & ~mask) | ((~s.out->doorbellAck) & mask);
  __asm__ volatile("dsb sy" ::: "memory");
  ringGlobalDoorbell(g.csg);
  gpuWrite(kDoorbell0 + 0x10000 * (g.csg + 1), 1);
}""")
sub("""    default:
      return kIOReturnUnsupported;
  }
}""", """    case MALI_GROUP_SUBMIT_BATCH: {   // akita: several streams of one group; out.done says how many went in (the rest: one by one)
      NEED_IN(mali_group_submit_batch_in, b);
      Group *g = groupFor(b.group);
      if (!g || !b.count || b.count > MALI_SUBMIT_BATCH_MAX) return kIOReturnBadArgument;
      for (unsigned e = 0; e < b.count; ++e) {
        const mali_submit_entry &x = b.e[e];
        if (x.queue >= g->queues || !x.cs_size || (x.cs_size & 7) || (x.cs_va & 63) || x.cs_va >= g->ctx->userVaRange || x.wait_count > MALI_SUBMIT_MAX_WAITS)
          return kIOReturnBadArgument;
        for (unsigned w = 0; w < x.wait_count; ++w) if (x.waits[w].queue >= g->queues) return kIOReturnBadArgument;
      }
      const int act = groupActivate(*g);
      if (act > 0) return kIOReturnNotReady;
      if (act < 0) return kIOReturnNoResources;
      mali_group_submit_batch_out o = {};
      uint32_t mask = 0;
      for (; o.done < b.count; ++o.done) {
        const mali_submit_entry &x = b.e[o.done];
        if (!groupSubmit(*g, x.queue, x.cs_va, x.cs_size, x.latest_flush, &o.seqno[o.done], x.waits, x.wait_count, false)) break;
        mask |= 1u << x.queue;
      }
      kickStreams(*g, mask);
      gSubPh[7] += o.done;
      PUT_OUT(mali_group_submit_batch_out, o);
      return kIOReturnSuccess;
    }
    default:
      return kIOReturnUnsupported;
  }
}""")
sub("""      if (selector == MALI_GROUP_SUBMIT || selector == MALI_GROUP_DESTROY) gpuFaultScan(selector == MALI_GROUP_SUBMIT ? "after a submit" : "after a group destroy");""",
    """      static uint64_t faultScanAt;   // akita: after a submit at most every 5 ms (one MMIO read costs ~7.6 us)
      if (selector == MALI_GROUP_DESTROY) gpuFaultScan("after a group destroy");
      else if ((selector == MALI_GROUP_SUBMIT || selector == MALI_GROUP_SUBMIT_BATCH) && (gFaultScanGap || (nanoseconds_to_absolutetime(5000000ull, &gFaultScanGap), true)) && mach_absolute_time() - faultScanAt > gFaultScanGap) {
        faultScanAt = mach_absolute_time();
        gpuFaultScan("after a submit");
      }""")
sub("""uint64_t gSubPh[8];""", """uint64_t gFaultScanGap;   // akita: 5 ms of mach_absolute_time, set at the first submit
uint64_t gSubPh[8];""")

# ---- akita fullos181: never fill a ring to the last byte, wait (do not fail) for room, and say where a queue faulted.
# fullos180 lost WindowServer to a CS fatal 0x40 (CS_CONFIG_FAULT) on two queues with 256 jobs outstanding: exactly the 64 KiB ring
# of 256-byte jobs, full (the check allowed insert - extract == size). A submit with no room returned NoSpace -> VK_ERROR_DEVICE_LOST.
sub("""  if (qu.insert + kJobBytes - ro->extract > qu.ringSize) return false;""",
    """  if (qu.insert + kJobBytes - ro->extract >= qu.ringSize) return false;   // akita: one job of slack, never a full ring""")
sub("""      const uint64_t sub0 = cntvctNow(), kick0 = gSubPh[2];
      if (!groupSubmit(*g, i.queue, i.cs_va, i.cs_size, i.latest_flush, &o.seqno, i2.waits, i2.wait_count)) return kIOReturnNoSpace;""",
    """      const uint64_t sub0 = cntvctNow(), kick0 = gSubPh[2];
      if (!ringHasRoom(*g, i.queue)) return kIOReturnNotReady;   // akita: the queue's ring is full: externalMethod waits and calls again
      if (!groupSubmit(*g, i.queue, i.cs_va, i.cs_size, i.latest_flush, &o.seqno, i2.waits, i2.wait_count)) return kIOReturnNoSpace;""")
sub("""      for (; o.done < b.count; ++o.done) {
        const mali_submit_entry &x = b.e[o.done];
        if (!groupSubmit(""", """      for (; o.done < b.count; ++o.done) {
        const mali_submit_entry &x = b.e[o.done];
        if (!ringHasRoom(*g, x.queue)) break;   // akita: the rest goes one by one, which waits for room
        if (!groupSubmit(""")
sub("""// akita: kickStream for several streams of a group's slot at once""",
    """// akita: room for one more job in queue q's ring (one job of slack kept, as groupSubmit checks).
bool ringHasRoom(Group &g, unsigned q) {
  if (q >= g.queues || g.csg == kNoSlot) return true;   // groupSubmit refuses those on its own terms
  const volatile RingOutput *ro = reinterpret_cast<volatile RingOutput *>(gSlotRes[g.csg].iface[q].kva + 0x1000);
  return g.q[q].insert + kJobBytes - ro->extract < g.q[q].ringSize;
}

// akita: kickStream for several streams of a group's slot at once""")
sub("""        lastFault[slot][q] = fault; lastFatal[slot][q] = fatal;
        ++shown;""", """        lastFault[slot][q] = fault; lastFatal[slot][q] = fatal;
        ++shown;
        {   // akita: where it stopped: the stream's command pointer, the ring's extract/insert and the ring words around extract
          const volatile RingInput *ri = reinterpret_cast<volatile RingInput *>(gSlotRes[slot].iface[q].kva);
          const volatile RingOutput *ro = reinterpret_cast<volatile RingOutput *>(gSlotRes[slot].iface[q].kva + 0x1000);
          const Queue &qu = g->q[q];
          const uint64_t ext = ro->extract, ins = ri->insert;
          printf("mali:   queue %u: cmd %#llx blocked %u wait %#x, ring extract %#llx insert %#llx (driver insert %#llx, size %#x, %llu bytes queued)\\n", q,
                 static_cast<unsigned long long>(c.out->statusCmdPtr), c.out->statusBlockedReason, c.out->statusWait,
                 static_cast<unsigned long long>(ext), static_cast<unsigned long long>(ins), static_cast<unsigned long long>(qu.insert), qu.ringSize,
                 static_cast<unsigned long long>(qu.insert - ext));
          const uint64_t *ring = reinterpret_cast<const uint64_t *>(qu.ring.kva);
          const uint64_t at = ((ext & (qu.ringSize - 1)) / 8 + qu.ringSize / 8 - 8) % (qu.ringSize / 8);
          for (unsigned k = 0; k < 16; k += 4)
            printf("mali:   ring[%llu..]: %016llx %016llx %016llx %016llx\\n", static_cast<unsigned long long>((at + k) % (qu.ringSize / 8)),
                   static_cast<unsigned long long>(ring[(at + k) % (qu.ringSize / 8)]), static_cast<unsigned long long>(ring[(at + k + 1) % (qu.ringSize / 8)]),
                   static_cast<unsigned long long>(ring[(at + k + 2) % (qu.ringSize / 8)]), static_cast<unsigned long long>(ring[(at + k + 3) % (qu.ringSize / 8)]));
        }""")

MALI.write_text(s)

# ---- the user API: one more selector and its structures
u = (ORIG / 'mali_uapi.h').read_text()
old = '''  MALI_GOP,               // in: mali_gop_in, out: mali_gop_out (the display driver's GPU hand-off: mode, wait for a finished frame, complete it)
  MALI_SELECTOR_COUNT'''
new = '''  MALI_GOP,               // in: mali_gop_in, out: mali_gop_out (the display driver's GPU hand-off: mode, wait for a finished frame, complete it)
  MALI_EVENT_WAIT,        // in: mali_event_wait_in, out: mali_event_wait_out (akita: sleep until a job finished or the firmware raised an event)
  MALI_GROUP_SUBMIT_BATCH, // in: mali_group_submit_batch_in, out: mali_group_submit_batch_out (akita: several streams of one group)
  MALI_SELECTOR_COUNT'''
assert u.count(old) == 1
u = u.replace(old, new)
old = '''struct mali_fence_wait_out { uint64_t done_seqno; uint32_t signaled, pad; };
'''
new = '''struct mali_fence_wait_out { uint64_t done_seqno; uint32_t signaled, pad; };

// akita: the driver counts its events (a job that finished, a fault, a tiler that ran out) in a generation number. The call returns at once
// when the number is no longer `gen` (pass 0 the first time), else when it moves or timeout_us (at most a second) passes; either way it
// says what the number is now. A waiter takes the number, looks at its fences, and only then sleeps on that number: nothing is missed.
// interrupts 0: the driver has no job interrupt (it would only time out): the caller polls the syncobj pages as before.
struct mali_event_wait_in { uint64_t gen, timeout_us; };
struct mali_event_wait_out { uint64_t gen; uint32_t interrupts, pad; };
'''
assert u.count(old) == 1
u = u.replace(old, new)
old = '''#define MALI_MAP_SCANOUT 0x40000000u
'''
new = '''#define MALI_MAP_SCANOUT 0x40000000u
// akita: IOConnectMapMemory64 type: the GPU's LATEST_FLUSH_ID register page, read-only, uncached (Panthor's USER_FLUSH_ID_MMIO page)
#define MALI_MAP_FLUSH_ID 0x20000000u
'''
assert u.count(old) == 1
u = u.replace(old, new)
old = '''struct mali_group_submit_out { uint64_t seqno; };   // the job's number on that queue, counted from 1
'''
new = '''struct mali_group_submit_out { uint64_t seqno; };   // the job's number on that queue, counted from 1

// akita: the longer form of the submit: before the stream runs, the GPU waits until each listed queue of the same group has finished
// its job number seqno (SYNC_WAIT64 on the group's syncobj words), so the caller need not block until then.
#define MALI_SUBMIT_MAX_WAITS 3
struct mali_group_submit_wait { uint32_t queue, pad; uint64_t seqno; };
struct mali_group_submit2_in { struct mali_group_submit_in base; uint32_t wait_count, pad; struct mali_group_submit_wait waits[MALI_SUBMIT_MAX_WAITS]; };
// akita: MALI_GROUP_SUBMIT_BATCH: up to MALI_SUBMIT_BATCH_MAX streams of one group, in order; out.done of them went in, with their seqnos.
#define MALI_SUBMIT_BATCH_MAX 32
struct mali_submit_entry { uint32_t queue, latest_flush; uint64_t cs_va; uint32_t cs_size, wait_count; struct mali_group_submit_wait waits[MALI_SUBMIT_MAX_WAITS]; };
struct mali_group_submit_batch_in { uint32_t group, count; struct mali_submit_entry e[MALI_SUBMIT_BATCH_MAX]; };
struct mali_group_submit_batch_out { uint32_t done, pad; uint64_t seqno[MALI_SUBMIT_BATCH_MAX]; };
'''
assert u.count(old) == 1
u = u.replace(old, new)
for p in UAPI:
    p.write_text(u)
print('patched', MALI, len(s))
