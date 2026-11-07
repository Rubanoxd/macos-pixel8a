# macOS 26 en un Google Pixel 8a (akita), con GPU Mali acelerando WindowServer

Port experimental de **macOS 26.5 Tahoe (25F71)** a un **Google Pixel 8a** (“akita”, SoC Google Tensor G3 “zuma”,
GPU **Mali-G715 MC7**). El sistema completo arranca desde RAM (`fastboot boot`, sin flashear nada), llega al escritorio,
y desde el 8 de noviembre de 2026 **WindowServer compone la pantalla con la GPU Mali** a través de Metal → Vulkan → PanVK.

<p align="center">
  <img src="photos/macos-windowserver-gpu-lockscreen.jpg" width="320" alt="Pantalla de bloqueo de macOS compuesta por la GPU Mali del Pixel 8a">
  <img src="photos/macos-fullos-desktop-first.jpg" width="320" alt="Primer escritorio de macOS en el Pixel 8a">
</p>

> **English summary.** Experimental port of macOS 26.5 (25F71) to the Pixel 8a. The full OS boots from RAM to the desktop,
> and WindowServer now composites on the Mali-G715 GPU through Metal → metal2vulkan → Mesa PanVK, driven by a custom IOKit
> Mali CSF driver in the kernel. This repo holds our patches, drivers, the Metal plugin, tools and notes — no Apple binaries.

> ⚠️ **Proyecto de investigación, no un sistema de uso diario.** No incluye ningún binario de Apple ni el firmware de ARM:
> cada uno debe obtenerlos legalmente (IPSW de Apple, partición vendor de su propio teléfono).

## Estado

| Hito | Estado |
|---|---|
| Arranque de XNU (kernel DEVELOPMENT) en el Tensor G3, desde RAM con `fastboot boot` | ✅ |
| macOS Recovery con barra de menús, Utilidad de Discos, Terminal, Safari | ✅ |
| macOS completo hasta el escritorio (Finder, Dock, Ajustes del Sistema, Terminal) | ✅ |
| Driver IOKit propio para la Mali-G715 (CSF: firmware MCU, MMU, grupos de colas) | ✅ |
| Vulkan (Mesa PanVK portado a Darwin arm64e): compute y primer triángulo | ✅ |
| Metal sobre Vulkan (metal2vulkan): `MTLCreateSystemDefaultDevice` = “Mali-G715 MC7” | ✅ |
| **WindowServer componiendo con la GPU (compositor Metal de SkyLight)** | ✅ (8 nov 2026) |
| Cola de la Mali estable (hay atascos periódicos de ~15 s) | 🔧 en curso |
| Autologin tras reiniciar WindowServer, CPU de WindowServer | 🔧 pendiente |

Más fotos en [`photos/`](photos/): Recuperación, Utilidad de Discos, Terminal, Safari, el primer triángulo Vulkan…

## Cómo funciona la aceleración gráfica

```
WindowServer (SkyLight, CompositorMetal) ─┐
CoreAnimation / apps                      ├─ Metal.framework
                                          │    └─ NativeMaliMetal.bundle  (plugin Metal, /System/Library/Extensions)
                                          │         └─ NativeBridgeMetalFrontend (UniXNU) + metal2vulkan (Rust)
                                          │              └─ Mesa PanVK (libvulkan_panfrost.dylib, portado a macOS arm64e)
                                          │                   └─ MaliClient (IOUserClient)
kernel XNU ───────────────────────────────┘                        └─ Mali.cpp: driver CSF de la Mali-G715 + IOAccelerator
```

Lo que hizo falta para que WindowServer use la GPU (detalles en el código del plugin y en los parches del kernel):

1. **Descubrimiento de Metal**: el driver publica un `IOAccelerator` con `MetalPluginName = NativeMaliMetal`; Metal carga el
   bundle y llama a `initWithAcceleratorPort:`.
2. **WindowServer es binario de plataforma con validación de librerías y sandbox**: el kernel (con el boot-arg
   `akita-mali-platform=1`) marca como código de plataforma el bundle firmado ad-hoc y deja abrir `MaliClient` saltando
   el chequeo MACF y el filtro del sandbox.
3. **`[MTLDevice acceleratorPort]`**: CoreDisplay asocia cada pantalla al acelerador que devuelve este método. Nuestro device
   no pasa por el `init` interno de `_MTLDevice` y devolvía 0, así que la pantalla quedaba sin acelerador y SkyLight usaba
   `compositor_sw`. El plugin ahora responde con el servicio `IOAccelerator` real.
4. **`WSSystemCanCompositeWithMetal`**: en 25F71 esta comprobación de SkyLight se queda en “no” aunque hay device (causa aún
   sin aclarar). Como solución temporal, el plugin la marca como superada al cargarse dentro de WindowServer (solo si existe
   `/System/Volumes/Data/ak/ws-force-metal` y solo en 25F71, comprobando las instrucciones esperadas).

Para encontrar esto se leyó la memoria de WindowServer en vivo (`tools/ws*.c`, con el boot-arg `akita-tfp=1` que deja a root
obtener el puerto de lectura de cualquier tarea) y se desensambló SkyLight, QuartzCore y CoreDisplay de 25F71 con
[ipsw](https://github.com/blacktop/ipsw).

## Contenido del repositorio

| Ruta | Qué es |
|---|---|
| `patches/xnu-12377.121.6-akita.patch` | Cambios sobre XNU 12377.121.6 (incluye los de macOS-ForSheng y los de akita: rc.server del sistema completo, MaliClient, plataforma, `akita-tfp`…). Las claves SSH van como marcadores: genera las tuyas. |
| `patches/macOS-ForSheng-ca57db7.patch` | Cambios sobre [CrossDarwin/macOS-ForSheng](https://github.com/CrossDarwin/macOS-ForSheng) `ca57db7`. |
| `platform-akita/` | Plataforma akita para macOS-ForSheng: drivers Mali (CSF), UFS, RAM disk, entrada táctil/botones, ADT, empaquetado de la imagen. Va en `platform/akita/` del árbol de ForSheng. |
| `patches/mesa-2648b20-akita.patch` | Mesa main `2648b20`: PanVK en Darwin (kmod sobre MaliClient, memoria, caché). |
| `patches/UniXNU-a3faa4a.patch` | Cambios sobre [UniXNU](https://github.com/UniXNU/UniXNU) `a3faa4a`. |
| `plugin/` | `NativeMaliMetal.bundle`: plugin de Metal (frontend de UniXNU + metal2vulkan + PanVK). |
| `tools/` | Pruebas (Vulkan, Metal, `metaldiscover`) e inspección de WindowServer (`wsvars`, `wswatch`, `wssample`, `wsstack`, `wsfind`…). |
| `scripts/` | Construcción de la imagen (`build-fullos.sh`), ciclos de prueba (`metal-cycle.sh`, `ws-metal-cycle.sh`), shell/log por USB, etc. |
| `docs/` | Guía práctica, plan del sistema completo e investigaciones previas (en español). |

Los scripts tienen rutas absolutas de la máquina de desarrollo (`/Volumes/datos2/unixnu-akita/...`): adáptalas.

## Requisitos (resumen)

- Pixel 8a con bootloader desbloqueado. Se arranca siempre con `fastboot boot` (RAM); no hace falta flashear.
- IPSW de macOS 26.5 (25F71) de Apple para el sistema y el kernelcache de referencia.
- Firmware CSF de tu propio teléfono: `adb pull /vendor/firmware/mali_csffw-r54p2.bin` y
  `scripts/make-mali-csffw-inc.py` para generar `platform-akita/drivers/Mali/mali_csffw.inc` (no se redistribuye).
- macOS en Apple Silicon con Xcode para compilar XNU, Mesa (meson/ninja), Rust 1.98 (metal2vulkan con `-Z build-std`).

## Créditos

- [macOS-ForSheng](https://github.com/CrossDarwin/macOS-ForSheng) — base del arranque de macOS 26 en hardware no Apple.
- [UniXNU](https://github.com/UniXNU/UniXNU) — frontend nativo de Metal y puente metal2vulkan.
- [Mesa](https://gitlab.freedesktop.org/mesa/mesa) — PanVK / panthor.
- [nullarc](https://github.com/luuumi-dev/nullarc) — referencia de WindowServer con GPU sobre Vulkan.
- [ipsw](https://github.com/blacktop/ipsw) — análisis del dyld shared cache.
- Apple — XNU es código abierto bajo APSL 2.0. macOS, sus frameworks y su marca son de Apple; este proyecto no está afiliado a Apple ni a Google.

## Licencia

Código propio bajo **GPL-3.0** (ver `LICENSE`), compatible con macOS-ForSheng. Los parches sobre proyectos de terceros
mantienen la licencia de cada proyecto (XNU: APSL 2.0; Mesa: MIT; UniXNU y los drivers de `platform-akita`:
BSD-2-Clause-Patent, ver los `LICENSE` de cada carpeta). Ver `THIRD_PARTY.md`.
