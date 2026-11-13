# Boot time (Nov 13, 2026)

Measured with the ARM generic counter. The firmware chain does not reset it, so the kernel logs it at `arm_init`
("AKITA boot clock" in the retained log), and the USB gadget logs it next to the calendar time.

| | fullos247 | fullos249 |
|---|---|---|
| `fastboot boot` → kernel `arm_init` | ~3 min 25 s | **~25 s** |
| `fastboot boot` → USB gadget (IOKit up) | 4 min 33 s | **1 min 33 s** |
| `fastboot boot` → first desktop frame | ~6 min 30 s | **~3 min 14 s** (fullos250 with the warm shader cache: 2 min 37 s; fullos253: **2 min 03 s**) |

Where the time went:

- **DarwinBoot showed the previous boot's log on every boot.** It showed up to five pages for 30 s each, plus 6 s, or
  20 s when no log was found. That was useful during bring-up, before there was a USB or Wi-Fi log, but is pure waiting now.
  It is now shown only when the previous log mentions a panic (`panic(`, `Kernel stack`, `SError`), at 3 s a page
  (`patches/DarwinBoot-macos-akita-no-log-pauses.patch`). The RAM log is still saved for the kernel as before.
- **The UEFI was a DEBUG build.** That meant debug output and every freed buffer filled with 0xAF. A RELEASE build
  (`build-akita-uefi.sh RELEASE`) saved ~30 s.

Also from this round:

- **WindowServer's shader cache now persists.** `/private/var/folders` is on a third APFS volume, "Folders", in the
  persistent container. Before, it lived on the RAM Data side and was emptied every boot, together with WindowServer's m2v
  pipeline cache: 33 s of pipeline compiles each boot, including single 13 s stalls ("cache hit 0").
  Measured on the next boot (fullos250): WindowServer compiled its 98 pipelines in **154 ms** in total, against 33 s
  before; the longest took 7 ms, against 13 s. The cache answered 94 times.
- **Three daemons that respawned in a loop are disabled:** coresymbolicationd (64 restarts in 15 minutes), routined (50)
  and intelligenceplatformd (35, exit 9).

### rc.server (the boot script) is on the critical path

launchd starts WindowServer only once `/etc/rc.server` is done: it ended at 03:38:46 and WindowServer started at 03:38:47
(fullos251). Timing marks (`/private/var/tmp/akita-boot.txt`) showed 55 s in it, nearly all after the persistent
volumes were mounted. Two changes brought it to **19 s** (fullos253):

- **The Data template's `/Library` is copied without the trees that are then linked back.** These are Photos resources,
  `/Library/Apple`, WebServer, Documentation and iLifeMediaBrowser: 157 of its 172 MB were copied into RAM and deleted again on
  every boot. rsync excludes them now.
- **The persistent home is no longer `chown -R`'d every boot**: that took 6.4 s for 21k files and grows with use. `find`
  changes only what is not already akita's (1.2 s, usually nothing).
