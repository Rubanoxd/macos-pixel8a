# Plan: arrancar el macOS completo (no Recovery) en el Pixel 8a

Estado a 6 nov 2026, 00:00. Base: macOS 26.5 (25F71), kernel de macOS-ForSheng con los parches de akita, imagen `live3`/`live4`.

## Lo que ya tenemos
- En `userdata` (manifiesto de 4 ventanas): `OS` (BaseSystem), `ISC`, `MacOS` (sistema completo, contenedor APFS con un solo volumen «Macintosh HD», UUID `47719F50-5527-44A3-9F59-8170528D8C14`, **sin sellar y sin grupo de volúmenes**) y `SystemOS` (cryptex sellado, UUID del contenedor `39D59E60-…`).
- Los binarios del sistema completo pasan AMFI y la caché compartida se mapea: el kernel la acepta desde el volumen raíz o desde `/System/Volumes/Preboot/Cryptexes`.
- Las 5 políticas de dibujo por software, ya localizadas en la caché completa: `config/akita-fullos-ui-pages.json`.
- El parche del kernel que deja intactas las páginas que no son de Recovery.
- Control en vivo: log por UDP, teclado/ratón, capturas y SSH.

## Cómo arranca Recovery hoy (macOS-ForSheng)
- `/chosen` del ADT:
  - `osenvironment = recoveryos`;
  - `boot-uuid` y `apfs-preboot-uuid` = UUID de BaseSystem;
  - `secure-boot-hashes/system-volume-auth-blob` = *root hash* de BaseSystem (raíz autenticada);
  - `asmb/lp-sip0 = 0`.

  Lo genera `scripts/native_devtree.py: configure_native_text_state` desde `platform/akita/package-akita-macos.py`.
- NativeUfs (modo Recovery) publica las ventanas como discos; la raíz se elige por UUID.
- El tmpfs de `/System/Volumes/Data` lo monta el userland de Recovery, no el kernel.

## Qué cambia en el macOS completo
1. **Raíz:** `boot-uuid` = «Macintosh HD».
   - Hay que decidir si va **autenticada**: sellar el volumen (`apfs_sealvolume`, del ramdisk de restauración) y usar el `root_hash` de la IPSW, `Firmware/094-56453-087.dmg.aea.root_hash`.
   - O **no autenticada**, aprovechando que SIP está desactivado (`lp-sip0`). Hay que comprobar qué exige el kernel sin `system-volume-auth-blob`.
2. **Cryptex SystemOS antes de launchd:** launchd y todo el userland necesitan la caché de dyld del cryptex.
   - En un Mac lo injerta launchd con `graftdmg` y un ticket img4; aquí no hay ticket.
   - Opción A (kernel, al estilo UniXNU): antes de `load_init_program`, montar el volumen SystemOS sobre `/System/Volumes/Preboot/Cryptexes/OS` (con un tmpfs en `/System/Volumes/Preboot` para crear la ruta). Referencia: `UniXNU/patches/xnu-12377/offline-owner-systemos-mount.patch` y `zzyb-native-cryptex-os-view.patch`.
   - Opción B (imagen): copiar la caché al volumen del sistema en `/System/Library/dyld` (dyld y el kernel aceptan esa ruta). Exige rehacer y reflashear la ventana `MacOS`, con propietario root, así que necesita sudo en el Mac.
3. **Volumen de datos:** APFS monta Data por rol dentro del grupo de volúmenes del sistema (`KERNEL_MOUNT_DATAVOL`, `VFSIOC_MOUNT_BYROLE`). Nuestra imagen no tiene grupo.
   - Opción A: un tmpfs en `/System/Volumes/Data` montado por el kernel antes de launchd (como hace Recovery desde el userland). Hay que ver si los *firmlinks* (`/Users`, `/private/var`, `/Library`…) funcionan sin grupo.
   - Opción B: rehacer el contenedor con un volumen Data (rol D) agrupado con el sistema (`diskutil apfs addVolume … -role D -groupWith …`, necesita sudo) y una ventana con *overlay* volátil en RAM como la de ISC.
4. **Usuario y asistente:** precrear un usuario (`/private/var/db/dslocal/nodes/Default/users/*.plist`) y `/private/var/db/.AppleSetupDone`, o dejar que salga el asistente de configuración.
5. **Dibujo por software:** activar en el kernel la tabla `akita-fullos-ui-pages.json`, de modo que convivan las reglas de Recovery y las del sistema completo. Hoy la primera regla con el mismo desplazamiento gana; hay que cambiar `return` por `continue` al no coincidir.
6. **ADT y boot-args:** sin `osenvironment=recoveryos`; quitar los parches propios de Recovery (Springboard, `recoveryosd`); mantener `native-gop-display`, la entrada, el gadget USB y el log.

## Primer experimento propuesto (solo RAM, sin flashear)
Imagen con `boot-uuid` = «Macintosh HD», kernel que monta SystemOS y un tmpfs para Data antes de launchd, y log por UDP. Objetivo: ver en el log hasta dónde llega launchd del sistema completo (loginwindow, WindowServer) y qué servicios fallan. Luego, el asistente de configuración o el usuario precreado.

## Riesgos conocidos
- RAM: XNU solo usa 4 GB (`owned_memory`). Subirla mejoraría mucho el macOS completo.
- Servicios que esperan hardware de Mac (Secure Enclave, `mobileactivationd`, Wi‑Fi y Bluetooth) pueden bloquear loginwindow.
- Lentitud: el primer arranque de cada app tarda minutos (escaneo de fuentes, CPU).
