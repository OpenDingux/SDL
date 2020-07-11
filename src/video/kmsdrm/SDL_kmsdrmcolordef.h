#include "SDL_config.h"
#include "SDL_stdinc.h"

#include <libdrm/drm_fourcc.h>

#define KMSDRM_DEFAULT_COLOR_DEPTH 16
typedef struct drm_color_def {
    Uint32 four_cc;
    Uint32 bpp;
    Uint32 r_mask,  g_mask,  b_mask,  a_mask;
    Uint32 r_bits,  g_bits,  b_bits,  a_bits;
    Uint32 r_shift, g_shift, b_shift, a_shift;
} drm_color_def;

#define MAKE_RGBA(type, bpp, rbits, gbits, bbits, abits, rsh, gsh, bsh, ash) \
    drm_color_def KMSDRM_COLOR_##type = { \
        DRM_FORMAT_##type, bpp, \
        (0xFF >> (8-rbits)) << rsh, (0xFF >> (8-gbits)) << gsh, \
        (0xFF >> (8-bbits)) << bsh, (0xFF >> (8-abits)) << ash, \
        rbits, gbits, bbits, abits, rsh, gsh, bsh, ash, \
    }

/*       |   CODE |BPP| R| G| B| A| RS| GS| BS| AS| */
MAKE_RGBA(XRGB8888, 32, 8, 8, 8, 0, 16,  8,  0,  0);
MAKE_RGBA(RGB565,   16, 5, 6, 5, 0, 11,  5,  0,  0);
MAKE_RGBA(XRGB1555, 16, 5, 5, 5, 0, 10,  5,  0,  0);
MAKE_RGBA(XBGR8888, 32, 8, 8, 8, 0,  0,  8, 16,  0);
MAKE_RGBA(BGR565,   16, 5, 6, 5, 0,  0,  5, 11,  0);
MAKE_RGBA(XBGR1555, 16, 5, 5, 5, 0,  0,  5, 10,  0);

/** 
 * TODO:: Figure out if there's any extra information that would be useful to
 * have in this macro. 
 **/
#define MAKE_YUV(type, bpp) \
    drm_color_def KMSDRM_COLOR_##type = { \
       DRM_FORMAT_##type, bpp, \
    };

MAKE_YUV(YUYV, 16);

/* Provides information on how to configure color format. */
const drm_color_def *get_drm_color_def(int depth, int isyuv, Uint32 flags)
{
    /** 
     * TODO:: implement actual YUV rather than 8bpp emulation. Until then, 
     * isyuv is left unused.
     **/
    if (flags & SDL_SWIZZLEBGR) {
        switch(depth) {
        /* case  8: return &KMSDRM_COLOR_YUYV; */
        case 16: return &KMSDRM_COLOR_BGR565;
        case 15: return &KMSDRM_COLOR_XBGR1555;
        case 24:
        case 32: return &KMSDRM_COLOR_XBGR8888;
        default: return NULL;
        }
    } else {
        switch(depth) {
        case  8: return &KMSDRM_COLOR_YUYV;
        case 16: return &KMSDRM_COLOR_RGB565;
        case 15: return &KMSDRM_COLOR_XRGB1555;
        case 24:
        case 32: return &KMSDRM_COLOR_XRGB8888;
        default: return NULL;
        }
    }
}

void get_framebuffer_args(const drm_color_def *def, unsigned int handle, unsigned int pitch,
    Uint32 *handles, Uint32 *pitches, Uint32 *offsets)
{
    switch (def->four_cc)
    {
        case DRM_FORMAT_YUYV:
            offsets[0] = 0;
            /* fall-through */
        case DRM_FORMAT_RGB565:
        case DRM_FORMAT_XRGB1555:
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_BGR565:
        case DRM_FORMAT_XBGR1555:
        case DRM_FORMAT_XBGR8888:
            handles[0] = handle;
            pitches[0] = pitch;
            break;
    }
}
