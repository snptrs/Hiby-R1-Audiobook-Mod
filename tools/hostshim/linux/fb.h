/* Minimal host-only shim so ui.c/render.c can be typechecked on macOS. */
#ifndef _SHIM_LINUX_FB_H
#define _SHIM_LINUX_FB_H
#include <stdint.h>
struct fb_bitfield { uint32_t offset, length, msb_right; };
struct fb_var_screeninfo {
    uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    uint32_t bits_per_pixel, grayscale;
    struct fb_bitfield red, green, blue, transp;
    uint32_t nonstd, activate, height, width, accel_flags;
    uint32_t pixclock, left_margin, right_margin, upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len, sync, vmode, rotate, colorspace;
    uint32_t reserved[4];
};
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPAN_DISPLAY     0x4606
#define FBIOBLANK           0x4611
#define FB_BLANK_UNBLANK    0
#endif
