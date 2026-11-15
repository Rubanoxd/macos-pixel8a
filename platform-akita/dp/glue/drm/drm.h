#include "../../shim/akita_linux_shim.h"
#ifndef AKITA_DRM_H
#define AKITA_DRM_H
typedef u8 __u8; typedef u16 __u16; typedef u32 __u32; typedef u64 __u64; typedef s32 __s32; typedef s64 __s64; typedef s8 __s8; typedef s16 __s16;
#define DRM_IOCTL_BASE 0x64
#define DRM_COMMAND_BASE 0x40
struct drm_event { __u32 type; __u32 length; };
#define __user
#endif
