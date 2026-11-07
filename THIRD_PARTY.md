# Third-party notices

| Component | Upstream | License | In this repo |
|---|---|---|---|
| XNU 12377.121.6 | https://github.com/apple-oss-distributions/xnu | APSL 2.0 | `patches/xnu-12377.121.6-akita.patch` (diff only) |
| macOS-ForSheng | https://github.com/CrossDarwin/macOS-ForSheng | GPL-3.0 / MIT | `patches/macOS-ForSheng-ca57db7.patch`, `platform-akita/` builds on its tree |
| UniXNU | https://github.com/UniXNU/UniXNU | BSD-2-Clause-Patent | `patches/UniXNU-a3faa4a.patch`; the plugin links its Metal frontend |
| Mesa (PanVK) | https://gitlab.freedesktop.org/mesa/mesa | MIT | `patches/mesa-2648b20-akita.patch` (diff only) |

Not included, and not redistributable: any Apple binary (IPSW, kernelcache, dyld shared cache, frameworks, disassembly of them)
and the ARM Mali CSF firmware (`mali_csffw-r54p2.bin`, from the phone's vendor partition).
