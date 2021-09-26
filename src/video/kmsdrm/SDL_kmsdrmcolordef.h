#include "SDL_config.h"
#include "SDL_stdinc.h"

#include <libdrm/drm_fourcc.h>

#ifndef _SDL_kmsdrmcolordef_h
#define _SDL_kmsdrmcolordef_h

#define KMSDRM_DEFAULT_COLOR_DEPTH 16
typedef struct drm_color_def {
    Uint32 four_cc;
    Uint32 bpp;
    Uint32 r_mask,  g_mask,  b_mask,  a_mask;
    Uint32 r_bits,  g_bits,  b_bits,  a_bits;
    Uint32 r_shift, g_shift, b_shift, a_shift;
    float h_factor;
} drm_color_def;

/* Provides information on how to configure color format. */
const drm_color_def *get_drm_color_def(int depth, Uint32 flags);
/* Provides necessary arguments for drm framebuffer creation */
void get_framebuffer_args(const drm_color_def *def, unsigned int handle, unsigned int pitch,
    Uint16 height, Uint32 *handles, Uint32 *pitches, Uint32 *offsets);

#endif /* _SDL_kmsdrmcolordef_h */