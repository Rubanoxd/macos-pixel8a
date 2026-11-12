# Third-party notices

| Component | Upstream | License | In this repo |
|---|---|---|---|
| XNU 12377.121.6 | https://github.com/apple-oss-distributions/xnu | APSL 2.0 | `patches/xnu-12377.121.6-akita.patch` (diff only) |
| macOS-ForSheng | https://github.com/CrossDarwin/macOS-ForSheng | GPL-3.0 / MIT | `patches/macOS-ForSheng-ca57db7.patch`, `platform-akita/` builds on its tree |
| UniXNU (and its AdrenoMetal frontend/bridge) | https://github.com/UniXNU/UniXNU | BSD-2-Clause-Patent | `patches/UniXNU-a3faa4a.patch`; the plugin links its Metal frontend |
| metal2vulkan | pinned by UniXNU (`624bab39` + UniXNU's bridge patches) | see upstream | `patches/metal2vulkan-624bab39-akita-heap-diag.patch` (diff only) |
| MaliMetal darwin-drm | https://github.com/Ken-u (MaliMetal) | BSD-2-Clause-Patent | `darwin-drm/` (modified; license in `darwin-drm/LICENSE`) |
| Mesa (PanVK) | https://gitlab.freedesktop.org/mesa/mesa | MIT | `patches/mesa-2648b20-akita.patch` (diff only) |
| Google goodix_touch | https://android.googlesource.com/kernel/google-modules/touch/goodix_touch/ | GPL-2.0 | protocol reference only; `AkitaTouch.inc` is a new implementation |

Not included, and not redistributable: any Apple binary (IPSW, kernelcache, dyld shared cache, frameworks, disassembly of them)
and the ARM Mali CSF firmware (`mali_csffw-r54p2.bin`, from the phone's vendor partition).
