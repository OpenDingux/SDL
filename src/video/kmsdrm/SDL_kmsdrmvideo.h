/*
    SDL - Simple DirectMedia Layer
    Copyright (C) 1997-2012 Sam Lantinga

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

    Sam Lantinga
    slouken@libsdl.org
*/
#include "SDL_config.h"
#include "SDL_stdinc.h"

#include <sys/types.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <libdrm/drm.h>
#include <libdrm/drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#ifndef _SDL_kmsdrmvideo_h
#define _SDL_kmsdrmvideo_h

#include "../SDL_sysvideo.h"

/* Hidden "this" pointer for the video functions */
#define _THIS	SDL_VideoDevice *this

/* Private display data */
typedef struct drm_prop_storage {
    drmModeObjectProperties *props;
	drmModePropertyRes **props_info;
    Uint32 obj_id;
    Uint32 obj_type;
    struct drm_prop_storage *next;
} drm_prop_storage;

typedef struct drm_pipe {
    /* fb -> plane -> crtc -> encoder -> connector */
    Uint32 framebuffer;
    Uint32 plane;
    Uint32 crtc;
    Uint32 encoder;
    Uint32 connector;

    struct drm_pipe *next;
} drm_pipe;

typedef struct drm_prop_arg {
	Uint32 obj_id;
	Uint32 obj_type;
	char name[DRM_PROP_NAME_LEN+1];
	Uint32 prop_id;
	Uint32 prop_drm_id;
	Uint64 value;
	int optional;
} drm_prop_arg;

struct SDL_PrivateVideoData {
    SDL_Rect **vid_modes;

    int fd;
	Uint32 size;
	Uint32 handle;
	void *map;

	Uint32 fb;
    drmModeModeInfo mode;

    drm_pipe *first_pipe;
    drm_pipe *active_pipe;
    drm_prop_storage *first_prop_store;

    struct drm_mode_destroy_dumb req_destroy_dumb;
    struct drm_mode_create_dumb req_create;
    struct drm_mode_map_dumb req_map;

    drmModeCrtc *prev_crtc;
};

#define drm_vid_modes        (this->hidden->vid_modes)
#define drm_fd               (this->hidden->fd)
#define drm_size             (this->hidden->size)
#define drm_handle           (this->hidden->handle)
#define drm_map              (this->hidden->map)
#define drm_fb               (this->hidden->fb)
#define drm_mode             (this->hidden->mode)
#define drm_props_conn       (this->hidden->props_conn)
#define drm_props_crtc       (this->hidden->props_crtc)
#define drm_props_plane      (this->hidden->props_plane)
#define drm_first_prop_store (this->hidden->first_prop_store)
#define drm_first_pipe       (this->hidden->first_pipe)
#define drm_active_pipe      (this->hidden->first_pipe)
#define drm_req_destroy_dumb (this->hidden->req_destroy_dumb)
#define drm_req_create       (this->hidden->req_create)
#define drm_req_map          (this->hidden->req_map)
#define drm_prev_crtc        (this->hidden->prev_crtc)

#endif /* _SDL_kmsdrmvideo_h */