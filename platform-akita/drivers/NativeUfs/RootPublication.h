// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// Shared production lifecycle decisions; adapters own IOKit objects and waits.
struct UfsRootPlan { unsigned count = 0, order[4] = {}; };
static bool ufsBuildRootPlan (const char *const *names, unsigned count, UfsRootPlan &plan)
{
  plan = {};
  if (!names || !count || count > 4) return false;
#if defined(NATIVE_UFS_RECOVERY_ROOT) && NATIVE_UFS_RECOVERY_ROOT
  if (count == 1) {
    if (!names[0] || strcmp (names[0], "OS")) return false;
    plan.count = 1;
    return true;
  }
  // OS (the paired BaseSystem) and ISC, plus up to two extra windows (akita: the full macOS system and its
  // SystemOS cryptex), published as plain disks between ISC and the recovery root, which stays last.
  if (count < 2) return false;
  unsigned recoveryOs = count, isc = count;
  for (unsigned i = 0; i < count; ++i) {
    if (!names[i] || !names[i][0]) return false;
    for (unsigned j = 0; j < i; ++j) if (!strcmp (names[i], names[j])) return false;
    if (!strcmp (names[i], "OS")) recoveryOs = i;
    if (!strcmp (names[i], "ISC")) isc = i;
  }
  if (recoveryOs == count || isc == count) return false;
  plan.order[0] = isc;
  unsigned next = 1;
  for (unsigned i = 0; i < count; ++i) if (i != isc && i != recoveryOs) plan.order[next++] = i;
  plan.order[next] = recoveryOs;
  plan.count = count;
  return true;
#else
  if (count == 1) return false;
#endif
  unsigned system = count, os = count;
  for (unsigned i = 0; i < count; ++i) {
    if (!names[i] || !names[i][0]) return false;
    for (unsigned j = 0; j < i; ++j) if (!strcmp (names[i], names[j])) return false;
    if (!strcmp (names[i], "SystemOS")) system = i;
    if (!strcmp (names[i], "OS")) os = i;
  }
  if (system == count || os == count) return false;
  plan.order[0] = system;
  unsigned position = 1;
  for (unsigned i = 0; i < count; ++i) if (i != system && i != os) plan.order[position++] = i;
  plan.order[position] = os;
  plan.count = count;
  return true;
}
enum class UfsRootPhase { Complete, Prepare, Identity, Schedule, Start, Media };
struct UfsRootPublicationResult {
  UfsRootPhase phase;
  unsigned index;
  unsigned published;
};
template <class Ops> static void ufsDiscardPrepared (Ops &ops, unsigned count)
{
  while (count) ops.discard (--count);
}
template <class Ops>
static UfsRootPublicationResult ufsPrepareRootWindows (Ops &ops, unsigned count)
{
#if defined(NATIVE_UFS_RECOVERY_ROOT) && NATIVE_UFS_RECOVERY_ROOT
  constexpr unsigned minimum = 1;
#else
  constexpr unsigned minimum = 2;
#endif
  if (count < minimum || count > 4) return { UfsRootPhase::Prepare, 0, 0 };
  for (unsigned i = 0; i < count; ++i) {
    if (!ops.prepare (i)) {
      // Include a partially allocated/initialized current object.
      ufsDiscardPrepared (ops, i + 1);
      return { UfsRootPhase::Prepare, i, 0 };
    }
  }
  if (!ops.validate ()) {
    ufsDiscardPrepared (ops, count);
    return { UfsRootPhase::Identity, count, 0 };
  }
  if (!ops.schedule ()) {
    ufsDiscardPrepared (ops, count);
    return { UfsRootPhase::Schedule, count, 0 };
  }
  return { UfsRootPhase::Complete, count, 0 };
}
template <class Ops>
static UfsRootPublicationResult ufsPublishRootWindows (Ops &ops, const UfsRootPlan &plan)
{
  unsigned published = 0;
  for (unsigned position = 0; position < plan.count; ++position) {
    unsigned index = plan.order[position];
    bool started = ops.start (index);
    if (!started || !ops.wait (index)) {
      // Keep already-started objects alive: their asynchronous clients may
      // still own I/O. Only discard windows that have never been published.
      unsigned first = position + (started ? 1 : 0);
      for (unsigned p = plan.count; p > first; --p) ops.discard (plan.order[p - 1]);
      return { started ? UfsRootPhase::Media : UfsRootPhase::Start, index, published + (started ? 1U : 0U) };
    }
    ++published;
  }
  return { UfsRootPhase::Complete, plan.count, published };
}
// A same-UUID IOMedia is ready only when its parent traversal reaches the
// actual verified window. An invalid or overlong iterator fails closed.
template <class Iterator, class Entry>
static bool ufsMediaHasAncestor (Iterator &iterator, Entry *window)
{
  for (unsigned steps = 0; steps < 256 && iterator.isValid (); ++steps) {
    auto *entry = iterator.getNextObject ();
    if (!entry) return false;
    if (entry == window) return iterator.isValid ();
  }
  return false;
}
