# macOS on a Pixel 8a: a practical guide to what we learned

Notes from an experimental port of **macOS 26.5 (25F71)** to a **Google Pixel 8a** ("akita", Tensor G3 "zuma" SoC).
This guide covers the Recovery stage (October–early November 2026): reaching the **macOS Recovery** window with a menu bar
and driving it with the physical buttons. Full macOS, the GPU and touch came later (see the README and the other docs).
It is not a daily-use system: it is a research project.
This is what worked, what did not and why, for anyone trying something similar on another Android phone.

> ⚠️ **Before you start**
> - You need a phone with an **unlocked bootloader** that you do not mind losing: unlocking wipes everything.
> - Apple's software has its own license. **Do not redistribute Apple binaries** (kernelcaches, BaseSystem, etc.); everyone must get them from Apple.
> - All of this is experimental. You can leave the phone unbootable if you flash things without knowing what you are doing.

---

## 1. Projects we build on

| Project | What it does |
|---|---|
| **Mu-Silicium** | UEFI firmware for Android phones, started from the vendor bootloader (ABL). |
| **UniXNU** | Menu and loader that boots XNU kernels from UEFI. Includes generic ARM CPU and platform drivers (ArmCpu, ArmDTPlatform). |
| **DarwinBoot** | Loader that prepares memory and the Apple device tree (ADT) and jumps to the XNU kernel. |
| **macOS-ForSheng** (CrossDarwin) | Port of macOS 26 to another phone (Xiaomi "sheng"). Provides the patched XNU kernel, the UFS storage driver, the framebuffer display driver and Recovery patches. **The base of everything.** |
| **macOS-DroidVM** | Origin of the display driver (`DroidVMDisplay`), an IOMobileFramebuffer on top of a simple framebuffer. |

The general idea: **reuse macOS-ForSheng and port only the hardware-specific parts**: storage, CPUs, buttons, memory and watchdog.

---

## 2. Boot chain

```
ABL (Google's bootloader)
 └─ Mu-Silicium (UEFI, runs at EL2)
     └─ UniXNU menu  (unixnu.toml, timeout = 3)
         └─ DarwinBoot  (drops from EL2 to EL1, builds the ADT)
             └─ XNU (macOS 26.5 kernel) ─ our drivers ─ Recovery BaseSystem on the userdata partition
```

- Always tested with **`fastboot boot`**: it boots from RAM and touches no partition.
- Only one thing was flashed: the macOS root image in `userdata`, which wipes Android's data.
- Everything (firmware and kernel) goes inside the boot.img's *kernel* field.

---

## 3. Problems we hit and how we solved them

### 3.1 The ABL hangs with large payloads
- **Symptom:** with a ~155 MB boot.img the ABL hangs. With ~77 MB it boots fine.
- **Cause:** the ABL decompresses the *kernel* field with LZ4 and has a size limit.
- **Fix:** store the kernel in a "sparse" format: our own header (`AKSPRS01`), a bitmap and only the non-zero 4 KiB pages. DarwinBoot expands it in memory before jumping to the kernel.

### 3.2 EL2 → EL1
- Mu-Silicium hands over control at **EL2**, but XNU expects **EL1**.
- DarwinBoot sets `HCR_EL2`, `CNTHCTL`, `CPTR`, `SCTLR_EL1`, etc., and does an `ERET` to EL1h.
- **Watch out for secondary cores:** PSCI `CPU_ON` also starts them at EL2. XNU's `reset_vector` must be patched so they do the same drop to EL1.

### 3.3 Secondary cores get no interrupts (IPIs)
- **Symptom:** with 8 CPUs the log showed `ipi-sent=0` and the system was extremely slow.
- **Cause:** XNU sends no IPIs to a core that still has the `SIGPdisabled` bit. ArmCpu only clears it with a self-IPI.
- **Fix:** boot-arg **`armcpu-self-ipi=1`**. With working SMP, Recovery went from never reaching the UI to showing it in about 7–13 minutes.

### 3.4 The watchdog reboots the phone
- Mu-Silicium arms cluster 0's watchdog (`WTCNT` at 0x10060000 + 8); if nobody reloads it the phone reboots.
- **Fixes:**
  1. Reload it in DarwinBoot right before jumping to the kernel.
  2. A kernel thread that reloads it periodically (boot-arg `akita-wdt-pet-seconds`).
- **Useful side effect:** with the reboot mode set to *bootloader*, the watchdog sends the phone back to fastboot instead of Android.

### 3.5 The on-screen console was so slow it triggered the watchdog
- **Cause:** when scrolling, the console read the framebuffer word by word, and that memory is mapped as device memory.
- **Fix:** a write-only mode that never reads and wraps to the top at the bottom (`sheng-panel-wrap=1`).

### 3.6 Seeing logs without a serial port
- **RAM does not survive** a watchdog reboot. We checked: a log kept in RAM is lost.
- **Fix:** reserve the **last 32 MiB of the `userdata` partition** as a log window.
  - The UFS driver writes a snapshot of the log there every 3 seconds, with a `SYNCHRONIZE CACHE`.
  - On the next boot DarwinBoot reads that window and shows it on screen in 30-second pages.
- **Filter the noise:** without `logd`, Recovery floods the console with launchd and xpcproxy messages. We filter those lines in the kernel and in DarwinBoot.
- To read the screen from the computer we used a camera pointed at the phone and OBS's preview.

### 3.7 Physical buttons as keyboard and mouse
- **Pins** (ALIVE controller at `0x154D0000`, active low):

  | Button | Register | Bit |
  |---|---|---|
  | Volume up | `gpa6`, DAT `0x154D00A4` | 2 |
  | Volume down | `gpa4`, DAT `0x154D0084` | 1 |
  | Power | `gpa4`, DAT `0x154D0084` | 0 |

- **Driver:** `NativeRecoveryInput` reads the pins every 20 ms and publishes two HID devices: a keyboard and a "boot" mouse. They use the same vendor/product ID as macOS-ForSheng's `IOHIDEventDriver` personality.
- **Keyboard mode:** volume = arrows, power = Enter. The arrows move through lists.
- **Note:** Recovery's windows **ignore Enter**, so a mouse is needed.
- **Pointer mode** (hold both volume buttons for 1 second to enable it):
  - volume up moves the cursor;
  - a short press of volume down changes direction;
  - power clicks.

### 3.8 Skipping the language picker
- The language picker does not respond to Enter.
- **Fix:** write the NVRAM variable **`prev-lang:kbd`** (for example `es:87` for Spanish) into the ADT's CHRP NVRAM bank. Recovery takes the language as chosen and skips that screen.

### 3.9 "Web browser only" instead of Recovery's utilities
- **Symptom:** after the language, Recovery opened only a browser (*guest* mode).
- **Cause:** macOS-ForSheng forces Recovery's boot mode to `guest` (its *recovery-guest-fallback* policy). It needed that for the language picker to appear.
  - Without that policy Recovery launches **Boot Picker**, but on our hardware the screen stays black.
- **Fix:** keep `guest` and patch **Recovery Springboard** in memory, with macOS-ForSheng's page mechanism. The `runningSafariMode` value is always stored as false.
  - Result: the **macOS Recovery** window appears, with *Next*, *Restart* and the menu bar *Recovery / File / Edit / Utilities / Window*.

### 3.10 "Examining locked volumes..." instead of the utilities
- **Symptom:** the *macOS Recovery* window stayed forever on "Examining locked volumes..." with a spinner.
- **Cause:** Recovery Springboard launches **KeyRecoveryAssistant** along two paths:
  1. `_runningActivationLockUnbrickMode` = true → patched with `strb wzr` on the ivar (see 3.9).
  2. `authenticatedToShowUtilities` returns false → `_launchKeyRecoveryAssistantForAuthentication`.
  - The second path was still active. In `showWindow` (VA 0x1000029e4), after calling `authenticatedToShowUtilities` (stub 0x10000c8c0), a `tbz w0, #0, 0x100002a9c` jumps to the authentication branch.
- **Fix:** patch the `tbz` with a **NOP** (`d503201f`) in page policy 11 (offset 10784 = 0x2a20, `springboard-skip-authentication-check`). Springboard always takes the "show utilities" path.
  - Result: the **four real utilities** appear: *Restore from Time Machine*, *Reinstall macOS Tahoe*, *Web Browser*, *Disk Utility*, with the *Continue* button.

### 3.11 Display: ghost cursors and invisible menus
- **Symptom:** moving the mouse left old cursors on screen, and menus did not show.
- **What we saw:**
  - WindowServer uses **a single** IOSurface for the desktop.
  - With a cursor or menus it sends **several layers** at once (`swap mask=0x7`).
  - The display driver could only show one "compatible" layer and showed **nothing** on those frames (`scanout layer=-1`).
- **Update (Nov 2026, 540×1200 mode + diff copy):** with a script that opens the *Utilities* menu and moves the cursor, every swap arrives with **a single layer** (`mask 0x1`) and the driver copies the changes: ~70,000 pixels when opening the menu, a few hundred when moving the cursor, no dropped frames. Menus and the cursor are drawn into layer 0.
- **Timing note:** with 8 CPUs `NativeRecoveryInput` starts ~4 minutes after `fastboot boot`, so a script with `W720000` acts 16–17 minutes after boot.

### 3.12 UI too small to read through the camera (HiDPI)
- **Problem:** at 1080×2400 the Recovery UI is tiny through the camera.
- **Fix:** the display driver publishes a mode smaller than the panel (boot-arg **`native-gop-mode-width=540`** → 540×1200, height keeps the ratio). WindowServer draws at that size and the driver scales each frame up to the physical 1080×2400. With 540 everything is twice as large and the utilities window still fits.
- Board patch: `platform/akita/patches/display-0002-akita-mode-scale.patch`. (Later the desktop moved to a 1200×540 landscape mode, `display-0011`.)

### 3.13 Clicking the UI without touching the phone (input script)
- `NativeRecoveryInput` publishes a third HID device, an absolute pointer, and reads a script from the boot-arg **`akita-input-script=STEP_STEP_...`**:
  - `W<ms>` waits, `A<x>,<y>` moves the pointer, `C` clicks, `D`/`U` button down/up, `K<hex>` presses a key, `P<x>,<y>` places the mouse cursor, `M` clicks with the mouse.
  - `K<usage>,<modifiers>` presses a combination (HID usage in hex; modifiers `02` shift, `04` option, `08` command). Example: `K17,0a` = ⇧⌘T.
  - `T<text>` types text with the Spanish layout: `+` is a space and `%XX` the ASCII character `XX` (for `_`, `+` or `%`). Example: `Tdf+-h_K28` types `df -h` and presses Return.
  - Coordinates are in **mode points** (with 540×1200, at that size), so they do not depend on the camera.
- **Terminal without the menu:** in Recovery, *Utilities → Terminal* has the shortcut **⇧⌘T** (from `Recovery Springboard.app/Contents/Resources/Utilities.plist`).
- Return does work in the utilities window: it activates *Continue* with the selected option (Time Machine by default).
- **The *Utilities* menu was disabled:** Recovery Springboard's `validateMenuItem:` only enables `launchUtility:` items if `authenticatedToShowUtilities` is YES. Since we skip authentication, Terminal and the rest of the menu (and its ⇧⌘T shortcut) did nothing. Fix: a fourth edit in policy 11, `springboard-enable-utility-menu` (offset 12536 of page 0: `b.eq` → `nop`, so it always returns YES; `config/macos26-recovery-ui-pages.json`).
- **Result (Nov 5, 2026):** ⇧⌘T opens **Terminal** (bash, as root) and the script runs commands: `diskutil list`, `df -h`, `id` (`uid=0(root)`).
- **Pitfall:** a `_` inside `T` ends the step. Write it as `%5f` (`sw%5fvers`).
- **Results:**

| Command | Output |
|---|---|
| `sysctl hw.ncpu hw.memsize hw.model` | 8 CPUs, 4294967296 bytes (4 GiB for macOS), model `akita-first-boot` |
| `mount` | `/` = `disk3s1` (sealed APFS, read-only); ISC on `disk1s1` (xarts), `disk1s2` (iSCPreboot), `disk1s3` (Hardware); `/System/Volumes/Data` and `/Volumes` on tmpfs |
| `ifconfig -l` | `lo0 gif0 stf0`: no network |
| `ps ax \| wc -l` | 91 processes |
| `nvram -p` | empty `boot-args` and `prev-lang:kbd es:87` |
| `sysctl machdep.cpu.brand_string hw.perflevel0.physicalcpu` | "ARM processor"; a single performance level with 8 cores |
| `ioreg -rd1 -c IOPlatformExpertDevice` | `google,akita`, compatible `google,akita` / `google,zuma` |
| `top`, `vm_stat` | not in BaseSystem ("command not found") |

- **`akita-input-keepawake=240`** moves the mouse one pixel every 4 minutes: otherwise macOS turns the display off after ~10 minutes idle and the panel keeps the last frame frozen.

### 3.14 Disk Utility stuck on "Loading disks"
- **Symptom:** the app opened, but with no disks and a spinner forever.
- **How we found it:**
  1. launchd's log showed **`fskitd`** (FSKit) exiting with `exit(1)` every few seconds.
  2. Capturing its stderr (`sheng-stderr-name=fskitd`) showed `fskitd: error starting up the daemon`.
  3. In the binary, that happens when `[[LiveFSUserClient defaultClient] setMainMachPort:forDomain:]` fails: `fskitd` needs the **`com.apple.filesystems.lifs`** (LiveFS) kext, which macOS-ForSheng's security relink did not include.
- **Fix:** add `--include-module com.apple.filesystems.lifs` to `emit_macos26_security.py`. Disk Utility now shows *macOS Base System*.
- **Useful tool:** the boot-arg **`sheng-errno-trace=<process>`** (board patches 0008/0009) writes the process's last failed syscalls to the log every 20 s as `syscall:errno` pairs.

### 3.15 Log-reading pitfalls
- DarwinBoot shows the previous boot's log **on every boot**, right after the UniXNU menu, in up to 5 pages of 30 s.
- Any image with `sheng-log=1` **overwrites** that log as soon as its kernel starts. To read it, boot an image without `sheng-log=1`.
- DarwinBoot drops lines with launchd phrases (`com.apple.xpc.launchd`, `failed lookup`, ...). Board patch 0007 shortens them in the kernel so they show.
- An XNU patch already applied cannot be edited (`build_macos26_xnu.py` rejects it): changes go in a new stacked patch.

### 3.16 USB device mode: the Pixel as the Mac's network card
- **Goal:** SSH into the phone over the same cable. BaseSystem ships `sshd`, `ssh`, `scp`, `nc`, `curl` and `script` (no `ssh-keygen` or `/etc/ssh`).
- **Hardware (zuma):** DWC_usb31 1.10a controller at `0x11210000`; USB subcontrol at `0x11100000` (registers `0x00`–`0x1c` only); eUSB2 PHY control at `0x11110000` (`0x00`–`0x20`); power domain `pd_hsi0` at PMU `0x15462A80`; CMU_HSI0 at `0x11000000`. Addresses from Google's DT and `google-modules/soc/gs`.
- **Pitfalls that hang the phone:**
  - Reading the PMU directly: on Tensor it is protected. Read it with `SMC 0x82000504` (x1 = physical address, x2 = 0), like Linux's `tensor_sec_reg_read`.
  - Reading undefined addresses of the USB subcontrol (for example `0x11100020`).
- **What the bootloader leaves:** the DWC3 powered, clocked and in device mode; VBUS/BVALID forced (`UTMI_CTRL = 3`); CMU clocks on. But the **eUSB2 PHY off** (`RST_CTRL = 3`, `CMN_CTRL = 0x502`, `TESTSE = 0x40`). With only Run/Stop the link stays "disconnected".
- **Fix:** repeat zuma's `phy_exynos_eusb_initiate` (19.2 MHz reference: `ref_freq_sel 0`, `pll_fb_div 368`, `pll_ref_div 0`; clear `test_iddq`; `phy_enable`; release the resets). The eUSB2 repeater (I2C `0x3E`) is already active from the bootloader.
- **Driver:** `platform/akita/input/AkitaUsbGadget.h`, inside the input kext (boot-arg `akita-usb-gadget=1`).
  - Event buffer and TRBs in contiguous uncached memory. Events are polled every millisecond, without an interrupt.
  - Endpoint 0 state machine like `dwc3/ep0.c`.
  - **CDC-ECM** function (`0525:a4a2`).
- **macOS blocks new USB accessories** ("device functionality blocked by transport restrictions") until the user clicks *Allow*. Then `AppleUserECM` loads and an interface appears with a link-local IP.
- **Stage 2 (`akita-usb-gadget=2`): real networking and SSH (Nov 5, 2026).**
  - The kext creates the BSD interface **`en0`** with the `ifnet` KPI (like XNU's `feth`), without IONetworkingFamily or configd, and sets **`169.254.7.2/16`** from the kernel.
  - From the Mac, **`./akita-usb-shell.sh setup`** leaves a pty shell on port 2222 and starts **`sshd`** with our own keys. Then: **`ssh -F scratch/sshkeys/ssh_config akita`**.
  - To copy files: `scp -O` (Recovery has no `sftp-server`).

### 3.17 Full-macOS apps inside the Recovery desktop
- **Layout:** `userdata` holds 4 windows: `OS` (BaseSystem), `ISC`, `MacOS` (full 25F71 system) and `SystemOS` (cryptex). NativeUfs publishes the last two as extra disks. Generator: `platform/akita/make-akita-fullroot.py`.
- **Chroot:** the cryptex is mounted at `System/Volumes/Preboot/Cryptexes/OS` of "Macintosh HD", with devfs and a tmpfs on `/private`. Full-system binaries pass AMFI and the shared cache maps.
- **Pitfalls:**
  - Recovery's UI patches panicked on full-cache pages. Foreign pages are now left untouched (`platform/sheng/macos26/recovery/ui_policy_kernel.h`).
  - Recovery only has `Fonts/Base`: the 83 full fonts are linked and fontd is replaced by the full system's.
  - The private SF Symbol `magnifyingglass.and.chevron.down` returns nil and crashes any `NSSearchField`: `nilguard.dylib`, injected with `DYLD_INSERT_LIBRARIES`, substitutes it.
- **Result (Nov 5, 2026):** the full macOS Terminal and **System Information** work ("macOS 26.5 (25F71)").

---

## 4. General advice

1. **Test in RAM, flash the minimum.** `fastboot boot` is your best friend, and the watchdog in bootloader mode brings you back to fastboot by itself.
2. **Set up a persistent log channel as early as possible.** Without a serial port, a log on storage read on the next boot saves days.
3. **One change per test.** Each boot to Recovery takes more than 10 minutes, so write down what you change and why.
4. **Beware of "innocent" fastboot commands.** On this phone `fastboot oem last_kmsg` left fastboot hung.
5. **When something fails in macOS, look in the reference port's code first.** Many "oddities", such as guest mode, were deliberate choices for other hardware.
6. **Verify what an AI model tells you**, including this document: register values change between hardware revisions.
7. **Work on an external disk.** XNU builds, the KDK and the macOS images take tens of GB.

---

## 5. Useful boot-args of this port

| Boot-arg | Purpose |
|---|---|
| `cpus=8 sheng-smp=1 armcpu-self-ipi=1` | SMP with the 8 cores and their IPIs. |
| `sheng-panel-wrap=1` | Fast on-screen console, no framebuffer reads. |
| `sheng-log=1` | Persistent log in the UFS window. |
| `akita-wdt-pet-seconds=N` | The kernel reloads the watchdog for N seconds. |
| `native-ufs-root=1 native-ufs-isc-overlay=1` | Use the BaseSystem on UFS as root. |
| `native-gop-display=1` | Framebuffer display driver. |
| `sheng-recovery-ui=1` | Enable macOS-ForSheng's Recovery page policies. |
| `sheng-recovery-ui-skip=0x20` | Disable a specific policy: each bit is a policy. |
| `native-recovery-input=1` | Physical buttons as keyboard and mouse. |
| `native-gop-mode-width=540` | 540×1200 display mode: UI at twice the size. |
| `akita-input-script=...` | Movement and click script (see 3.13). |
| `akita-input-keepawake=240` | Keeps macOS from turning the display off when idle. |
| `sheng-stderr-log=2 sheng-stderr-name=<process>` | Copies that process's stdout/stderr to the log. |
| `sheng-errno-trace=<process>` | Logs that process's failed syscalls. |
| `akita-mali-platform=1` | Lets WindowServer load the Mali Metal plugin and open the GPU (full OS). |
| `akita-touch=1` | Touchscreen driver (full OS). |

---

If you do something similar on another phone, sections 3.2 to 3.6 are the most reusable. The Recovery parts (3.8 to 3.11)
apply to any hardware running the same macOS-ForSheng.
