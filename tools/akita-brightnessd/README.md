# akita-brightnessd

macOS's brightness (Control Center slider, brightness keys) for the Pixel 8a panel.

How macOS 26 drives a built-in panel's backlight, as found in its frameworks:
- BrightnessControl.framework (`BCAppleBacklightBrtControl`) takes any IOService with `backlight-control` = true, reads
  `IODisplayParameters` (`brightness` and `BrightnessMilliNits`, each {min, max, value}) and sets the level with
  `setProperties {"brightness-nits": nits in 16.16}`. setNits is refused (error 16) unless the service has
  `force-brightness-control` (data) or is a Touch Bar.
- The kernel's AkitaBacklightService (platform-akita/drivers/Acpm/AkitaBacklight.inc) is that service: it maps nits to the
  panel's DCS 0x51 level over DSIM0.
- QuartzCore only hands brightness to BrightnessControl after IOMobileFramebuffer pairs the framebuffer with it
  (`IOMobileFramebufferSetBrightnessControlCallback`), which our framebuffer does not meet. CoreBrightness still keeps the
  user's brightness (`DisplayServicesGetBrightness`), so this agent watches it and writes `brightness-nits` itself.

Build: `clang -O2 -arch arm64e -mmacosx-version-min=26.0 -framework CoreFoundation -framework CoreGraphics -framework IOKit
akita-brightnessd.c -o akita-brightnessd`, ad-hoc sign, install at ~/Library/Akita and load com.akita.brightnessd.plist as
a LaunchAgent. `bctest.m` asks BrightnessControl what it makes of the service.
