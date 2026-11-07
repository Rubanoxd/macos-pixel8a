# macOS en un Pixel 8a: guía práctica de lo aprendido

Notas de un port experimental de **macOS 26.5 Recovery (25F71)** a un **Google Pixel 8a** ("akita", SoC Tensor G3 "zuma").
Hemos llegado a la ventana **Recuperación de macOS** con barra de menús, en español, y a manejarla con los botones físicos.
No es un sistema usable día a día: es un proyecto de investigación.
Esto es lo que funcionó, lo que no y por qué, para quien quiera intentar algo parecido en otro móvil Android.

> ⚠️ **Antes de empezar**
> - Hace falta un móvil con el **bootloader desbloqueado** y que no te importe perder: desbloquear lo borra todo.
> - El software de Apple tiene su licencia. **No redistribuyas binarios de Apple** (kernelcaches, BaseSystem, etc.); cada uno debe obtenerlos de Apple.
> - Todo esto es experimental. Puedes dejar el móvil sin arrancar si flasheas cosas sin saber lo que haces.

---

## 1. Proyectos en los que nos apoyamos

| Proyecto | Para qué sirve |
|---|---|
| **Mu-Silicium** | Firmware UEFI para móviles Android, que se arranca desde el bootloader del fabricante (ABL). |
| **UniXNU** | Menú y cargador que arranca kernels XNU desde UEFI. Incluye drivers genéricos de CPU y plataforma para ARM (ArmCpu, ArmDTPlatform). |
| **DarwinBoot** | Cargador que prepara la memoria, el device tree de Apple (ADT) y salta al kernel XNU. |
| **macOS-ForSheng** (CrossDarwin) | Port de macOS 26 a otro móvil (Xiaomi "sheng"). Aporta el kernel XNU compilado con parches, el driver de almacenamiento UFS, el driver de pantalla por framebuffer y parches de Recovery. **La base de todo.** |
| **macOS-DroidVM** | Origen del driver de pantalla (`DroidVMDisplay`), un IOMobileFramebuffer encima de un framebuffer simple. |

La idea general: **reutilizar macOS-ForSheng y portar solo lo específico del hardware**: almacenamiento, CPUs, botones, memoria y watchdog.

---

## 2. Cadena de arranque

```
ABL (bootloader de Google)
 └─ Mu-Silicium (UEFI, corre en EL2)
     └─ menú UniXNU  (unixnu.toml, timeout = 3)
         └─ DarwinBoot  (baja de EL2 a EL1, monta el ADT)
             └─ XNU (kernel de macOS 26.5) ─ drivers propios ─ BaseSystem de Recovery en la partición userdata
```

- Se prueba **siempre con `fastboot boot`**: arranca desde RAM y no toca ninguna partición.
- Solo se flasheó una cosa: la imagen raíz de macOS en `userdata`, que borra los datos de Android.
- Todo (firmware y kernel) va dentro del campo *kernel* del boot.img.

---

## 3. Problemas que encontramos y cómo los resolvimos

### 3.1 El ABL se cuelga con payloads grandes
- **Síntoma:** con un boot.img de unos 155 MB, el ABL se queda colgado. Con unos 77 MB arranca bien.
- **Causa:** el ABL descomprime el campo *kernel* con LZ4 y tiene un límite de tamaño.
- **Solución:** guardar el kernel en formato "sparse": cabecera propia (`AKSPRS01`), un mapa de bits y solo las páginas de 4 KiB que no son cero. DarwinBoot lo expande en memoria antes de saltar al kernel.

### 3.2 EL2 → EL1
- Mu-Silicium entrega el control en **EL2**, pero XNU espera **EL1**.
- DarwinBoot configura `HCR_EL2`, `CNTHCTL`, `CPTR`, `SCTLR_EL1`, etc., y hace `ERET` a EL1h.
- **Ojo con los núcleos secundarios:** PSCI `CPU_ON` también los arranca en EL2. Hay que parchear el `reset_vector` de XNU para que hagan la misma bajada a EL1.

### 3.3 Los núcleos secundarios no reciben interrupciones (IPIs)
- **Síntoma:** con 8 CPUs, el log mostraba `ipi-sent=0` y el sistema iba lentísimo.
- **Causa:** XNU no envía IPIs a un núcleo que aún tiene el bit `SIGPdisabled`. ArmCpu solo lo limpia con una auto-IPI.
- **Solución:** boot-arg **`armcpu-self-ipi=1`**. Con SMP funcionando, Recovery pasó de no llegar nunca a la interfaz a mostrarla en unos 7–13 minutos.

### 3.4 El watchdog reinicia el móvil
- Mu-Silicium arma el watchdog del cluster 0 (`WTCNT` en 0x10060000 + 8), y si nadie lo recarga el móvil se reinicia.
- **Soluciones:**
  1. Recargarlo en DarwinBoot justo antes de saltar al kernel.
  2. Un hilo en el kernel que lo recarga periódicamente (boot-arg `akita-wdt-pet-seconds`).
- **Efecto útil:** con el modo de reinicio en *bootloader*, el watchdog devuelve el móvil a fastboot en vez de a Android.

### 3.5 La consola en pantalla era tan lenta que disparaba el watchdog
- **Causa:** al desplazar el texto, la consola leía el framebuffer palabra a palabra, y esa memoria está mapeada como dispositivo.
- **Solución:** un modo de escritura que solo escribe, sin leer, y vuelve arriba al llegar abajo (`sheng-panel-wrap=1`).

### 3.6 Cómo ver logs sin puerto serie
- **La RAM no sobrevive** al reinicio del watchdog. Lo comprobamos: un log guardado en RAM se pierde.
- **Solución:** reservar los **últimos 32 MiB de la partición `userdata`** como ventana de log.
  - El driver UFS escribe allí una instantánea del log cada 3 segundos, con su `SYNCHRONIZE CACHE`.
  - En el siguiente arranque, DarwinBoot lee esa ventana y la enseña en pantalla en páginas de 30 segundos.
- **Filtra el ruido:** sin `logd`, Recovery inunda la consola con mensajes de launchd y xpcproxy. Filtramos esas líneas en el kernel y en DarwinBoot.
- Para leer la pantalla desde el ordenador usamos una cámara apuntando al móvil y la vista previa de OBS.

### 3.7 Los botones físicos como teclado y ratón
- **Pines** (controlador ALIVE en `0x154D0000`, activos a nivel bajo):

  | Botón | Registro | Bit |
  |---|---|---|
  | Volumen arriba | `gpa6`, DAT `0x154D00A4` | 2 |
  | Volumen abajo | `gpa4`, DAT `0x154D0084` | 1 |
  | Encendido | `gpa4`, DAT `0x154D0084` | 0 |

- **Driver:** `NativeRecoveryInput` lee los pines cada 20 ms y publica dos dispositivos HID: un teclado y un ratón "boot". Usan el mismo vendor/product ID que la personalidad `IOHIDEventDriver` de macOS-ForSheng.
- **Modo teclado:** volumen = flechas, encendido = Enter. Las flechas mueven listas.
- **Ojo:** las ventanas de Recovery **ignoran Enter**, así que hace falta ratón.
- **Modo puntero** (mantener los dos volúmenes 1 segundo para activarlo):
  - volumen arriba mueve el cursor;
  - volumen abajo, con una pulsación corta, cambia la dirección;
  - encendido hace clic.

### 3.8 Saltarse el selector de idioma
- El selector de idioma no responde a Enter.
- **Solución:** escribir la variable NVRAM **`prev-lang:kbd`** (por ejemplo `es:87` para español) en el banco NVRAM CHRP del ADT. Recovery da el idioma por elegido y se salta esa pantalla.

### 3.9 "Solo navegador web" en vez de las utilidades de Recovery
- **Síntoma:** después del idioma, Recovery abría solo un navegador (modo *invitado*).
- **Causa:** macOS-ForSheng fuerza el modo de arranque de Recovery a `guest` (su política *recovery-guest-fallback*). Lo necesitaba para que apareciera el selector de idioma.
  - Si se quita esa política, Recovery lanza **Boot Picker** (las opciones de arranque), pero en nuestro hardware la pantalla se queda en negro.
- **Solución:** mantener `guest` y parchear en memoria **Recovery Springboard**, con el mismo mecanismo de páginas de macOS-ForSheng. El valor `runningSafariMode` se guarda siempre como falso.
  - Resultado: la ventana **Recuperación de macOS** aparece, con *Siguiente*, *Reiniciar* y la barra de menús *Recuperación / Archivo / Edición / Utilidades / Ventana*.

### 3.10 "Examinando volúmenes bloqueados..." en vez de las utilidades
- **Síntoma:** la ventana *Recuperación de macOS* se quedaba para siempre en "Examinando volúmenes bloqueados..." con un spinner.
- **Causa:** Recovery Springboard lanza **KeyRecoveryAssistant** por dos caminos:
  1. `_runningActivationLockUnbrickMode` = true → parcheado con `strb wzr` en el ivar (ver 3.9).
  2. `authenticatedToShowUtilities` devuelve false → `_launchKeyRecoveryAssistantForAuthentication`.
  - El segundo camino seguía activo. En `showWindow` (VA 0x1000029e4), tras llamar a `authenticatedToShowUtilities` (stub 0x10000c8c0), un `tbz w0, #0, 0x100002a9c` salta a la rama de autenticación.
- **Solución:** parchear el `tbz` con un **NOP** (`d503201f`) en la política de página 11 (offset 10784 = 0x2a20, `springboard-skip-authentication-check`). Springboard siempre cae al camino de "mostrar utilidades".
  - Resultado: las **cuatro utilidades reales** aparecen: *Restaura desde Time Machine*, *Reinstala macOS Tahoe*, *Navegador Web*, *Utilidad de Discos*, con el botón *Continuar*.

### 3.11 Pantalla: cursores fantasma y menús que no aparecen (en curso)
- **Síntoma:** al mover el ratón quedan cursores viejos en pantalla, y los menús no se ven.
- **Lo que hemos visto:**
  - WindowServer usa **una sola** IOSurface para el escritorio.
  - Cuando hay cursor o menús, manda **varias capas** a la vez (`swap mask=0x7`).
  - El driver de pantalla solo sabe mostrar una capa "compatible" y, en esos fotogramas, **no muestra nada** (`scanout layer=-1`).
- **Siguiente paso:** averiguar en qué parte del registro de swap van la posición y el tamaño de cada capa, para componerlas todas (escritorio + menús + cursor) en el framebuffer.
- **Actualización (noviembre de 2026, modo 540×1200 + copia por diferencias):** con un guion que abre el menú *Utilidades* y mueve el cursor, todos los swaps llegan con **una sola capa** (`mask 0x1`) y el driver copia los cambios: unos 70.000 píxeles al abrir el menú, unos cientos al mover el cursor, sin fotogramas perdidos. La familia nunca llama a `surface_layer_map` con superficies extra. En este flujo los menús y el cursor se dibujan en la capa 0; falta confirmarlo a ojo con la cámara enfocando la parte de arriba de la pantalla.
- **Ojo con los tiempos:** con 8 CPUs, `NativeRecoveryInput` arranca unos 4 minutos después del `fastboot boot`, así que un guion con `W720000` actúa a los 16–17 minutos del arranque.

### 3.12 Interfaz demasiado pequeña para leerla con la cámara (HDPI)
- **Problema:** a 1080×2400 la interfaz de Recovery es diminuta vista por la cámara.
- **Solución:** el driver de pantalla publica a macOS un modo más pequeño que el panel (boot-arg **`native-gop-mode-width=540`** → 540×1200, la altura conserva la proporción). WindowServer dibuja en ese tamaño y el driver amplía cada fotograma a los 1080×2400 físicos con el escalador que ya tenía. Con 540 todo se ve al doble y la ventana de utilidades sigue cabiendo.
- Parche de placa: `platform/akita/patches/display-0002-akita-mode-scale.patch`.

### 3.13 Pulsar en la interfaz sin tocar el móvil (script de entrada)
- `NativeRecoveryInput` publica un tercer dispositivo HID, un puntero absoluto, y lee un guion del boot-arg **`akita-input-script=PASO_PASO_...`**:
  - `W<ms>` espera, `A<x>,<y>` mueve el puntero, `C` hace clic, `D`/`U` botón abajo/arriba, `K<hex>` pulsa una tecla, `P<x>,<y>` coloca el cursor del ratón, `M` clic con el ratón.
  - `K<uso>,<modificadores>` pulsa una combinación (uso HID en hex; modificadores `02` mayúsculas, `04` opción, `08` comando). Ejemplo: `K17,0a` = ⇧⌘T.
  - `T<texto>` escribe texto con la distribución española: `+` es un espacio y `%XX` el carácter ASCII `XX` (para `_`, `+` o `%`). Ejemplo: `Tdf+-h_K28` escribe `df -h` y pulsa Intro.
  - Las coordenadas van en **puntos del modo** (con 540×1200, en ese tamaño), así que no dependen de la cámara.
- **Terminal sin menú:** en Recovery, *Utilidades → Terminal* tiene el atajo **⇧⌘T** (sale de `Recovery Springboard.app/Contents/Resources/Utilities.plist`). En ese menú Terminal es la tercera opción, después de *Utilidad de Seguridad de Arranque* y *Asistente de Recuperación*.
- Intro sí funciona en la ventana de utilidades: activa *Continuar* con la opción seleccionada (Time Machine por defecto).
- **El menú *Utilidades* estaba desactivado:** `validateMenuItem:` de Recovery Springboard solo activa las opciones de `launchUtility:` si `authenticatedToShowUtilities` es YES. Como nos saltamos la autenticación, Terminal y el resto del menú (y su atajo ⇧⌘T) no hacían nada. Arreglo: cuarta edición en la política 11, `springboard-enable-utility-menu` (desplazamiento 12536 de la página 0: `b.eq` → `nop`, así que siempre devuelve YES; `config/macos26-recovery-ui-pages.json`, nuevo `after_sha256` af67e9d9…). Imagen de prueba: `out/akita-macos-term3`.
- **Resultado (5 de noviembre de 2026):** con esa edición, ⇧⌘T abre el **Terminal** (bash, como root) y el guion ejecuta comandos. Se leen la salida de `diskutil list` (contenedor APFS de 2,1 GB, volumen *macOS Base System* de 1,5 GB), `df -h` y `id` (`uid=0(root)`). Fotos: `scratch/milestones/macos-terminal-pixel8a-*.jpg`.
- **Guion de diagnóstico** (`out/akita-macos-term5`): `W600000`, ⇧⌘T dos veces separadas 30 s (si se pulsa antes de que el menú esté listo, no hace nada y el Intro siguiente abre Time Machine), ⌘+ tres veces (`K30,08`: letra más grande, 59 columnas) y ⌘K (`K0e,08`) para borrar la pantalla entre comandos.
- **Trampa:** un `_` dentro de `T` termina el paso. Hay que escribirlo como `%5f` (`sw%5fvers`).
- **Resultados:**

| Comando | Salida |
|---|---|
| `sysctl hw.ncpu hw.memsize hw.model` | 8 CPUs, 4294967296 bytes (4 GiB para macOS), modelo `akita-first-boot` |
| `mount` | `/` = `disk3s1` (APFS sellado, solo lectura); ISC en `disk1s1` (xarts), `disk1s2` (iSCPreboot), `disk1s3` (Hardware); `/System/Volumes/Data` y `/Volumes` en tmpfs |
| `ifconfig -l` | `lo0 gif0 stf0`: sin red |
| `kextstat` | llama a `kmutil showloaded`, que avisa de que «no variant specified» y solo devuelve 2 líneas |
| `ps ax \| wc -l` | 91 procesos |
| `nvram -p` | `boot-args` vacío y `prev-lang:kbd es:87` |
| `sysctl machdep.cpu.brand_string hw.perflevel0.physicalcpu` | «ARM processor»; un solo nivel de rendimiento con 8 núcleos |
| `ioreg -rd1 -c IOPlatformExpertDevice` | `google,akita`, compatible `google,akita` / `google,zuma`, modelo `akita-first-boot` |
| `top`, `vm_stat` | no existen en BaseSystem («command not found») |

- Tras ⌘K a veces quedan restos de un prompt anterior: son zonas que el driver de pantalla no repinta.
- Coordenadas útiles en 540×1200: fila *Utilidad de Discos* (232,639), botón *Continuar* (480,712). La interfaz tarda unos 12–13 minutos en estar lista, así que el guion empieza con `W720000`.
- **`akita-input-keepawake=240`** mueve el ratón un píxel cada 4 minutos: si no, macOS apaga la pantalla a los ~10 minutos de inactividad y el panel se queda con la última imagen congelada.

### 3.14 Utilidad de Discos se quedaba en "Cargando discos"
- **Síntoma:** la app se abría, pero sin discos y con un spinner para siempre.
- **Cómo lo encontramos:**
  1. El log de launchd mostró que **`fskitd`** (FSKit) salía con `exit(1)` cada pocos segundos.
  2. Capturando su stderr (`sheng-stderr-name=fskitd`) apareció `fskitd: error starting up the daemon`.
  3. En el binario, eso ocurre cuando `[[LiveFSUserClient defaultClient] setMainMachPort:forDomain:]` falla: `fskitd` necesita el kext **`com.apple.filesystems.lifs`** (LiveFS), y el relink de seguridad de macOS-ForSheng no lo incluía.
- **Solución:** añadir `--include-module com.apple.filesystems.lifs` a `emit_macos26_security.py`. Utilidad de Discos muestra ya *macOS Base System* (APFS, solo lectura, 1,58 GB).
- **Herramienta útil:** el boot-arg **`sheng-errno-trace=<proceso>`** (parches de placa 0008/0009) escribe en el log, cada 20 s, las últimas syscalls fallidas de ese proceso como pares `syscall:errno`.

### 3.15 Trampas al leer el log
- DarwinBoot enseña el log del arranque anterior **en cada arranque**, justo después del menú de UniXNU, en hasta 5 páginas de 30 s.
- Cualquier imagen con `sheng-log=1` **sobrescribe** ese log en cuanto arranca su kernel. Para leerlo, arranca una imagen sin `sheng-log=1`.
- DarwinBoot descarta las líneas con frases de launchd (`com.apple.xpc.launchd`, `failed lookup`, ...). El parche de placa 0007 las acorta en el kernel para que sí se vean.
- Un parche XNU ya aplicado no se puede editar (`build_macos26_xnu.py` lo rechaza): los cambios van en un parche nuevo apilado.


### 3.16 USB en modo dispositivo: el Pixel como tarjeta de red del Mac
- **Objetivo:** conectarse por SSH a Recovery por el mismo cable. El BaseSystem trae `sshd`, `ssh`, `scp`, `nc`, `curl` y `script` (no trae `ssh-keygen` ni `/etc/ssh`).
- **Hardware (zuma):** controlador DWC_usb31 1.10a en `0x11210000`; subcontrol USB en `0x11100000` (solo registros `0x00`–`0x1c`); control del PHY eUSB2 en `0x11110000` (`0x00`–`0x20`); dominio `pd_hsi0` en el PMU `0x15462A80`; CMU_HSI0 en `0x11000000`. Las direcciones salen del DT de Google y de `google-modules/soc/gs` (rama `android-gs-akita-6.1-android15-qpr2`: `cal-if/zuma/cmucal-sfr.c`, `phy/samsung/phy-exynos-eusb.c`, `exynos-usb-blkcon*`).
- **Trampas que cuelgan el teléfono:**
  - Leer el PMU directamente: en Tensor está protegido. Se lee con `SMC 0x82000504` (x1 = dirección física, x2 = 0), igual que `tensor_sec_reg_read` en Linux.
  - Leer direcciones no definidas del subcontrol USB (por ejemplo `0x11100020`).
- **Lo que deja el bootloader:** el DWC3 encendido, con reloj y en modo dispositivo; VBUS/BVALID forzados (`UTMI_CTRL = 3`); los relojes del CMU encendidos. Pero el **PHY eUSB2 apagado** (`phy_exynos_eusb_terminate`: `RST_CTRL = 3`, `CMN_CTRL = 0x502`, `TESTSE = 0x40`). Con solo Run/Stop, el enlace se queda en «desconectado».
- **Arreglo:** repetir `phy_exynos_eusb_initiate` para zuma (referencia de 19,2 MHz: `ref_freq_sel 0`, `pll_fb_div 368`, `pll_ref_div 0`; quitar `test_iddq`; `phy_enable`; soltar los resets). El repetidor eUSB2 (I2C `0x3E`) ya viene activo del bootloader.
- **Driver:** `platform/akita/input/AkitaUsbGadget.h`, dentro del kext de entrada (boot-arg `akita-usb-gadget=1`; arranca 60 s después del driver).
  - Búfer de eventos y TRBs en memoria contigua sin caché (`kIOMapWriteCombineCache`). Los eventos se sondean cada milisegundo, sin interrupción.
  - Máquina de estados del endpoint 0 como la de `dwc3/ep0.c`.
  - Función **CDC‑ECM** (`0525:a4a2`, «Pixel 8a macOS Recovery»).
  - Etapa 1: el propio driver contesta ARP e ICMP para `169.254.7.2`.
- **macOS bloquea los accesorios USB nuevos** («device functionality blocked by transport restrictions») hasta que el usuario pulsa *Permitir*. Después carga `AppleUserECM` y aparece una interfaz (`en13`) con IP link-local.
- **Resultado (5 nov 2026):** `ping -b en13 169.254.7.2` responde en unos 1 ms, también con tramas de 1514 bytes y de 1024 bytes (múltiplo del paquete USB, que el driver rellena con un byte).
- **Etapa 2 (`akita-usb-gadget=2`): red de verdad y SSH (5 nov 2026).**
  - El kext crea la interfaz BSD **`en0`** con la KPI `ifnet` (como `feth` de XNU: `ifnet_allocate_extended` + `ether_demux`/`ether_frameout_extended`), sin IONetworkingFamily ni configd. Pone la IP **`169.254.7.2/16`** desde el kernel (`SIOCAIFADDR` en un socket de kernel).
  - El guion de teclas abre el Terminal (⇧⌘T) y escribe `nc -l 2323|bash|nc -l 2324`: se evitan `<` y `>`, difíciles con la distribución española.
  - Desde el Mac, **`./akita-usb-shell.sh setup`** deja una shell con pty en el puerto 2222 (`nc` + `script`) y arranca **`sshd`** con claves propias (`scratch/sshkeys`). Después: **`ssh -F scratch/sshkeys/ssh_config akita`**.
  - Para copiar archivos: `scp -O` (Recovery no trae `sftp-server`).
  - Imagen: `out/akita-macos-gadget2`. Trampa al enlazar: `bpfattach` sale con nombre C++ porque su cabecera no lleva `extern "C"`, así que se quitó.
- **Para leer el log sin hacer fotos:** `screencapture -l<id de la ventana de OBS>` y recortar/girar con `sips`.

### 3.17 Apps del macOS completo dentro del escritorio de Recovery
- **Montaje:** `userdata` lleva ahora 4 ventanas: `OS` (BaseSystem), `ISC`, `MacOS` (sistema completo 25F71) y `SystemOS` (cryptex). NativeUfs publica las dos últimas como discos extra en Recovery. Generador: `platform/akita/make-akita-fullroot.py`.
- **Chroot:** el cryptex va montado en `System/Volumes/Preboot/Cryptexes/OS` de «Macintosh HD», con devfs y un tmpfs en `/private`. La primera vez, dyld necesita la caché en `/System/Library/dyld`; se pone con una unión temporal y un proceso ancla la mantiene mapeada. Los binarios del sistema completo pasan AMFI y la caché compartida se mapea.
- **Trampas:**
  - Los parches de interfaz de Recovery entraban en pánico con las páginas de la caché completa. Ahora las ajenas se dejan intactas (`platform/sheng/macos26/recovery/ui_policy_kernel.h`).
  - Recovery solo trae `Fonts/Base`: se enlazan las 83 fuentes completas y se sustituye fontd (`gui/0/com.apple.xtyped`) por el del sistema completo, con `RootDirectory`.
  - El SF Symbol privado `magnifyingglass.and.chevron.down` devuelve nil y tumba cualquier `NSSearchField`: `nilguard.dylib`, inyectada con `DYLD_INSERT_LIBRARIES`, lo sustituye.
  - Sin LaunchServices, las apps SwiftUI no crean su ventana: la misma librería llama a `applicationShouldHandleReopen` y la trae al frente.
  - Los paneles de Ajustes son extensiones que no descubre el LaunchServices/pkd de Recovery, así que la barra lateral sale vacía.
- **Resultado (5 nov 2026):** el Terminal y la **Información del Sistema** del macOS completo funcionan («macOS 26.5 (25F71)», volumen de arranque «Macintosh HD»). Ajustes del Sistema abre su ventana, pero sin paneles.
- **Herramientas:** `akita-boot.sh <imagen>` (arranque, log y SSH), `akita-fullos.sh "<app>"` (prepara el chroot y lanza la app), `akita-live.py` (log, teclado/ratón y capturas) y `tools/chroot/`.

---

## 4. Consejos generales

1. **Prueba en RAM, flashea lo mínimo.** `fastboot boot` es tu mejor amigo, y el watchdog en modo bootloader te devuelve solo a fastboot.
2. **Monta un canal de log persistente lo antes posible.** Sin puerto serie, un log en el almacenamiento que se lea en el siguiente arranque te ahorra días.
3. **Un cambio por prueba.** Cada arranque hasta Recovery tarda más de 10 minutos, así que apunta qué cambias y por qué.
4. **Cuidado con los comandos "inocentes" de fastboot.** En este móvil, `fastboot oem last_kmsg` dejaba fastboot colgado.
5. **Cuando algo no funcione en macOS, busca primero en el código del port de referencia.** Muchas "rarezas", como el modo invitado, eran decisiones deliberadas para otro hardware.
6. **Verifica lo que te diga un modelo de IA**, incluido este documento: los valores de registros cambian entre revisiones de hardware.
7. **Trabaja en un disco externo.** Las compilaciones de XNU, el KDK y las imágenes de macOS ocupan decenas de GB.

---

## 5. Boot-args útiles de este port

| Boot-arg | Para qué |
|---|---|
| `cpus=8 sheng-smp=1 armcpu-self-ipi=1` | SMP con los 8 núcleos y sus IPIs. |
| `sheng-panel-wrap=1` | Consola en pantalla rápida, sin lecturas del framebuffer. |
| `sheng-log=1` | Log persistente en la ventana UFS. |
| `akita-wdt-pet-seconds=N` | El kernel recarga el watchdog durante N segundos. |
| `native-ufs-root=1 native-ufs-isc-overlay=1` | Usa como raíz el BaseSystem que está en UFS. |
| `native-gop-display=1` | Driver de pantalla por framebuffer. |
| `sheng-recovery-ui=1` | Activa las políticas de página de Recovery de macOS-ForSheng. |
| `sheng-recovery-ui-skip=0x20` | Desactiva una política concreta: cada bit es una política. |
| `native-recovery-input=1` | Botones físicos como teclado y ratón. |
| `native-gop-mode-width=540` | Modo de pantalla 540×1200: interfaz al doble de tamaño. |
| `akita-input-script=...` | Guion de movimientos y clics (ver 3.13). |
| `akita-input-keepawake=240` | Evita que macOS apague la pantalla por inactividad. |
| `sheng-stderr-log=2 sheng-stderr-name=<proceso>` | Copia al log lo que ese proceso escribe por stdout/stderr. |
| `sheng-errno-trace=<proceso>` | Registra las syscalls fallidas de ese proceso. |

---

## 6. Estado actual (noviembre de 2026)

| Pieza | Estado |
|---|---|
| Arranque de XNU con 8 CPUs | ✅ |
| Almacenamiento UFS (raíz de Recovery) | ✅ |
| Log persistente en UFS | ✅ |
| Interfaz gráfica de Recovery (selector de idioma, ventana *Recuperación de macOS*) | ✅ |
| Utilidades de Recovery (Time Machine, Reinstalar, Navegador Web, Utilidad de Discos) | ✅ |
| Utilidad de Discos con la lista de discos (requiere el kext lifs) | ✅ |
| Terminal de Recovery (⇧⌘T por guion, comandos escritos con `T`) | ✅ |
| Interfaz al doble de tamaño (modo 540×1200) | ✅ |
| Botones como teclado y ratón | ✅ |
| Guion de entrada automático (puntero absoluto) | ✅ |
| Composición de capas de pantalla (menús y cursor) | ⏳ en curso |
| Pantalla táctil | ❌ El controlador Goodix va por SPI y hace falta un driver propio. |
| USB en modo dispositivo (CDC‑ECM; ping desde el Mac) | ✅ |
| Red de macOS por USB (`en0`, 169.254.7.2) y SSH desde el Mac | ✅ |
| Apps del macOS completo (Terminal, Información del Sistema) en el escritorio de Recovery | ✅ |
| Arranque del macOS completo hasta el escritorio | 🚧 |
| Wi-Fi, GPU | ❌ |

Si haces algo parecido en otro móvil, lo más reutilizable son las secciones 3.2 a 3.6. Lo de Recovery (3.8 a 3.11) sirve para cualquier hardware que corra el mismo macOS-ForSheng.
